#include "platform/platform.h"
#include <errno.h>
#include <string.h>
#ifndef __ZEPHYR__
#include <netinet/tcp.h>
#include <poll.h>
#endif

#include "broker.h"
#include "client.h"
#include "topic.h"
#include "session.h"
#include "packet.h"
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
#include "p2p.h"
#endif

LOG_MODULE_REGISTER(mqtt_broker, LOG_LEVEL_INF);

static int listen_fd = -1;
static int extra_listen_fd = -1;
static char bind_host[64];
static uint16_t listen_port = MQTT_BROKER_PORT;
static uint16_t extra_listen_port;
static uint16_t admission_limit = MQTT_ADMISSION_MAX_CLIENTS;
static broker_activity_cb_t activity_cb;
static void *activity_ctx;
static int extra_listener_started;

#define MESH_INGRESS_MAX_CLIENTS 2
#define MESH_INGRESS_SUBS_PER_CLIENT 4
#define MESH_INGRESS_INFLIGHT_MAX 4
#define MESH_INGRESS_QOS2_IN_MAX 4
#define MESH_INGRESS_RETRY_MS 5000

typedef struct {
    uint16_t packet_id;
    uint8_t buf[MQTT_MAX_PACKET_SIZE + 8];
    uint16_t len;
    int64_t sent_at_ms;
    uint8_t in_use;
    uint8_t qos;
    uint8_t waiting_pubcomp;
} mesh_ingress_inflight_t;

typedef struct {
    uint16_t packet_id;
    mqtt_publish_t pub;
    uint8_t in_use;
    uint8_t published;
} mesh_ingress_qos2_in_t;

typedef struct {
    int fd;
    uint8_t in_use;
    uint16_t next_packet_id;
    char subs[MESH_INGRESS_SUBS_PER_CLIENT][MQTT_TOPIC_MAX];
    uint8_t qos[MESH_INGRESS_SUBS_PER_CLIENT];
    uint8_t sub_in_use[MESH_INGRESS_SUBS_PER_CLIENT];
    mesh_ingress_inflight_t inflight[MESH_INGRESS_INFLIGHT_MAX];
    mesh_ingress_qos2_in_t qos2_in[MESH_INGRESS_QOS2_IN_MAX];
} mesh_ingress_client_t;

static mesh_ingress_client_t mesh_clients[MESH_INGRESS_MAX_CLIENTS];

#if defined(CONFIG_MQTT_P2P_DYNAMIC)
static int mesh_filter_refcount(const char *filter)
{
    int count = 0;

    if (!filter || filter[0] == '\0') {
        return 0;
    }
    for (int i = 0; i < MESH_INGRESS_MAX_CLIENTS; i++) {
        if (!mesh_clients[i].in_use) {
            continue;
        }
        for (int j = 0; j < MESH_INGRESS_SUBS_PER_CLIENT; j++) {
            if (mesh_clients[i].sub_in_use[j] &&
                strcmp(mesh_clients[i].subs[j], filter) == 0) {
                count++;
            }
        }
    }
    return count;
}

static int mesh_filter_can_withdraw(const char *filter)
{
    return mesh_filter_refcount(filter) == 0 &&
           topic_filter_subscriber_count(filter) == 0;
}
#endif

int broker_set_bind_host(const char *host)
{
    if (!host || !host[0]) {
        bind_host[0] = '\0';
        return 0;
    }
    size_t len = strlen(host);
    if (len >= sizeof(bind_host)) {
        return -EINVAL;
    }
    memcpy(bind_host, host, len + 1);
    return 0;
}

int broker_set_listen_port(uint16_t port)
{
    if (port == 0) {
        return -EINVAL;
    }
    listen_port = port;
    return 0;
}

int broker_set_admission_limit(uint16_t max_clients)
{
    if (max_clients == 0) {
        admission_limit = MQTT_ADMISSION_MAX_CLIENTS;
        return 0;
    }
    if (max_clients > MQTT_MAX_CLIENTS) {
        return -EINVAL;
    }
    admission_limit = max_clients;
    return 0;
}

void broker_set_activity_callback(broker_activity_cb_t cb, void *ctx)
{
    activity_cb = cb;
    activity_ctx = ctx;
}

void broker_notify_activity(void)
{
    broker_activity_cb_t cb = activity_cb;

    if (cb) {
        cb(activity_ctx);
    }
}

static int mqtt_topic_match(const char *filter, const char *topic)
{
    const char *f = filter;
    const char *t = topic;

    while (*f && *t) {
        if (*f == '#') {
            return 1;
        }
        if (*f == '+') {
            while (*t && *t != '/') {
                t++;
            }
            f++;
        } else {
            if (*f != *t) {
                return 0;
            }
            f++;
            t++;
        }
    }
    if (*f == '#') {
        return 1;
    }
    if (*f == '/' && *(f + 1) == '#' && *(f + 2) == '\0' && *t == '\0') {
        return 1;
    }
    return *f == '\0' && *t == '\0';
}

static int broker_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0) {
        ssize_t n = plat_send(fd, p, len, 0);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void configure_client_socket(int fd)
{
#ifndef __ZEPHYR__
    int one = 1;
    int buf = 131072;

    (void)plat_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef TCP_QUICKACK
    (void)plat_setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
    (void)plat_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    (void)plat_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
#else
    ARG_UNUSED(fd);
#endif
}

static void reject_server_unavailable(int fd, const char *reason)
{
    uint8_t rej[4];
    int rlen;

    LOG_WRN("%s, sending CONNACK SERVER_UNAVAIL fd=%d", reason, fd);
    rlen = packet_build_connack(0, CONNACK_SERVER_UNAVAIL, rej, sizeof(rej));
    if (rlen > 0) {
        (void)broker_send_all(fd, rej, (size_t)rlen);
    }
    plat_close(fd);
}

static int open_listener(uint16_t port)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    const char *listen_host = bind_host[0] ? bind_host : "0.0.0.0";
    int fd;

    fd = plat_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        LOG_ERR("socket: %d", errno);
        return -errno;
    }

    int opt = 1;
    plat_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind_host[0] && plat_inet_pton(AF_INET, bind_host, &addr.sin_addr) != 1) {
        LOG_ERR("invalid bind host: %s", bind_host);
        plat_close(fd);
        return -EINVAL;
    }

    if (plat_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind: %d", errno);
        plat_close(fd);
        return -errno;
    }

    if (plat_listen(fd, MQTT_MAX_CLIENTS) < 0) {
        LOG_ERR("listen: %d", errno);
        plat_close(fd);
        return -errno;
    }

    LOG_INF("Listening on %s:%d (max %d clients, admission %u)",
            listen_host, port, MQTT_MAX_CLIENTS, admission_limit);
    return fd;
}

int broker_init(void)
{
    topic_init();
    session_init();
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    p2p_router_init();
#endif

    if (admission_limit > MQTT_MAX_CLIENTS) {
        admission_limit = MQTT_MAX_CLIENTS;
    }

    listen_fd = open_listener(listen_port);
    if (listen_fd < 0) {
        return listen_fd;
    }
    return 0;
}

int broker_stop_mqtt_listener(void)
{
    if (listen_fd < 0) {
        return 0;
    }
    plat_close(listen_fd);
    listen_fd = -1;
    LOG_INF("MQTT listener stopped port=%u", listen_port);
    return 0;
}

int broker_start_mqtt_listener(void)
{
    if (listen_fd >= 0) {
        return 0;
    }
    listen_fd = open_listener(listen_port);
    if (listen_fd < 0) {
        return listen_fd;
    }
    LOG_INF("MQTT listener restarted port=%u", listen_port);
    return 0;
}

static void broker_accept_one(int fd, uint16_t port)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);

    int client_fd = plat_accept(fd, (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) {
        LOG_WRN("accept: %d", errno);
        return;
    }
    configure_client_socket(client_fd);

    LOG_INF("New TCP connection fd=%d port=%u", client_fd, port);

    if (admission_limit < MQTT_MAX_CLIENTS &&
        client_used_slots() >= admission_limit) {
        reject_server_unavailable(client_fd, "Admission limit reached");
        return;
    }

    if (client_alloc(client_fd) < 0) {
        /* MQTT 3.1.1 §3.2.2.3: send SERVER_UNAVAILABLE then close */
        reject_server_unavailable(client_fd, "No free client slots");
    }
}

static int recv_all_timeout(int fd, uint8_t *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = plat_recv(fd, buf + done, len - done, 0);
        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int recv_mqtt_packet(int fd, mqtt_packet_t *out)
{
    uint8_t b;
    uint32_t rlen = 0;
    uint32_t mult = 1;

    if (!out || recv_all_timeout(fd, &b, 1) < 0) {
        return -1;
    }
    out->type_flags = b;
    do {
        if (recv_all_timeout(fd, &b, 1) < 0) {
            return -1;
        }
        rlen += (uint32_t)(b & 0x7f) * mult;
        mult *= 128;
        if (mult > 128U * 128U * 128U) {
            return -1;
        }
    } while (b & 0x80);

    if (rlen > MQTT_MAX_PACKET_SIZE) {
        return -1;
    }
    out->remaining_len = rlen;
    out->buf_len = rlen;
    if (rlen > 0 && recv_all_timeout(fd, out->buf, rlen) < 0) {
        return -1;
    }
    return 0;
}

static mesh_ingress_client_t *mesh_client_alloc(int fd)
{
    for (int i = 0; i < MESH_INGRESS_MAX_CLIENTS; i++) {
        if (!mesh_clients[i].in_use) {
            memset(&mesh_clients[i], 0, sizeof(mesh_clients[i]));
            mesh_clients[i].fd = fd;
            mesh_clients[i].in_use = 1;
            return &mesh_clients[i];
        }
    }
    return NULL;
}

static void mesh_client_close(mesh_ingress_client_t *c)
{
    if (!c || !c->in_use) {
        return;
    }
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    for (int i = 0; i < MESH_INGRESS_SUBS_PER_CLIENT; i++) {
        char filter[MQTT_TOPIC_MAX];

        if (!c->sub_in_use[i]) {
            continue;
        }
        strncpy(filter, c->subs[i], sizeof(filter) - 1);
        filter[sizeof(filter) - 1] = '\0';
        c->sub_in_use[i] = 0;
        if (mesh_filter_can_withdraw(filter)) {
            p2p_local_unsubscribe(filter);
        }
    }
#endif
    plat_close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static int mesh_client_subscribe(mesh_ingress_client_t *c, const char *filter,
                                 uint8_t qos)
{
    int free_slot = -1;
    if (!c || !filter || filter[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < MESH_INGRESS_SUBS_PER_CLIENT; i++) {
        if (c->sub_in_use[i] && strcmp(c->subs[i], filter) == 0) {
            c->qos[i] = qos;
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
            p2p_local_subscribe(filter, qos);
#endif
            return 0;
        }
        if (!c->sub_in_use[i] && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return -1;
    }
    strncpy(c->subs[free_slot], filter, MQTT_TOPIC_MAX - 1);
    c->qos[free_slot] = qos;
    c->sub_in_use[free_slot] = 1;
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    p2p_local_subscribe(filter, qos);
#endif
    return 0;
}

static int mesh_client_unsubscribe(mesh_ingress_client_t *c, const char *filter)
{
    int removed = 0;

    if (!c || !filter || filter[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < MESH_INGRESS_SUBS_PER_CLIENT; i++) {
        if (c->sub_in_use[i] && strcmp(c->subs[i], filter) == 0) {
            c->sub_in_use[i] = 0;
            removed = 1;
        }
    }
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    if (removed && mesh_filter_can_withdraw(filter)) {
        p2p_local_unsubscribe(filter);
    }
#endif
    return removed ? 0 : -1;
}

static void mesh_publish_from_ingress(const mqtt_publish_t *pub)
{
    if (!pub) {
        return;
    }
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    p2p_publish_from_local(pub);
#endif
    broker_mesh_ingress_publish_remote(pub);
    broker_notify_activity();
}

static int mesh_qos2_find(mesh_ingress_client_t *c, uint16_t packet_id)
{
    for (int i = 0; i < MESH_INGRESS_QOS2_IN_MAX; i++) {
        if (c->qos2_in[i].in_use && c->qos2_in[i].packet_id == packet_id) {
            return i;
        }
    }
    return -1;
}

static int mesh_qos2_alloc(mesh_ingress_client_t *c)
{
    for (int i = 0; i < MESH_INGRESS_QOS2_IN_MAX; i++) {
        if (!c->qos2_in[i].in_use) {
            return i;
        }
    }
    return -1;
}

static void mesh_inflight_store(mesh_ingress_client_t *c, uint16_t packet_id,
                                const uint8_t *buf, uint16_t len, uint8_t qos)
{
    if (!c || packet_id == 0 || !buf || len == 0) {
        return;
    }
    for (int i = 0; i < MESH_INGRESS_INFLIGHT_MAX; i++) {
        if (!c->inflight[i].in_use) {
            c->inflight[i].packet_id = packet_id;
            c->inflight[i].len = len;
            c->inflight[i].sent_at_ms = plat_uptime_ms();
            c->inflight[i].in_use = 1;
            c->inflight[i].qos = qos;
            c->inflight[i].waiting_pubcomp = 0;
            memcpy(c->inflight[i].buf, buf, len);
            return;
        }
    }
    LOG_WRN("mesh ingress inflight full, dropping QoS-%u id=%u", qos, packet_id);
}

static void mesh_inflight_ack(mesh_ingress_client_t *c, uint16_t packet_id)
{
    for (int i = 0; i < MESH_INGRESS_INFLIGHT_MAX; i++) {
        if (c->inflight[i].in_use && c->inflight[i].packet_id == packet_id) {
            c->inflight[i].in_use = 0;
            return;
        }
    }
}

static void mesh_inflight_pubrec(mesh_ingress_client_t *c, uint16_t packet_id)
{
    uint8_t rel[4];
    int len;

    for (int i = 0; i < MESH_INGRESS_INFLIGHT_MAX; i++) {
        if (!c->inflight[i].in_use ||
            c->inflight[i].packet_id != packet_id ||
            c->inflight[i].qos != 2 ||
            c->inflight[i].waiting_pubcomp) {
            continue;
        }
        len = packet_build_pubrel(packet_id, rel, sizeof(rel));
        if (len > 0 && broker_send_all(c->fd, rel, (size_t)len) == 0) {
            c->inflight[i].waiting_pubcomp = 1;
            c->inflight[i].sent_at_ms = plat_uptime_ms();
            memcpy(c->inflight[i].buf, rel, (size_t)len);
            c->inflight[i].len = (uint16_t)len;
        }
        return;
    }
}

static void mesh_inflight_retry(mesh_ingress_client_t *c)
{
    int64_t now = plat_uptime_ms();

    if (!c || !c->in_use) {
        return;
    }
    for (int i = 0; i < MESH_INGRESS_INFLIGHT_MAX; i++) {
        if (!c->inflight[i].in_use ||
            now - c->inflight[i].sent_at_ms < MESH_INGRESS_RETRY_MS) {
            continue;
        }
        if (!(c->inflight[i].qos == 2 && c->inflight[i].waiting_pubcomp)) {
            c->inflight[i].buf[0] |= 0x08;
        }
        c->inflight[i].sent_at_ms = now;
        if (broker_send_all(c->fd, c->inflight[i].buf, c->inflight[i].len) < 0) {
            mesh_client_close(c);
            return;
        }
    }
}

static void mesh_ingress_retry_all(void)
{
    for (int i = 0; i < MESH_INGRESS_MAX_CLIENTS; i++) {
        mesh_inflight_retry(&mesh_clients[i]);
    }
}

static uint16_t packet_id_from_ack(const mqtt_packet_t *pkt)
{
    if (!pkt || pkt->buf_len < 2) {
        return 0;
    }
    return (uint16_t)((pkt->buf[0] << 8) | pkt->buf[1]);
}

static void mesh_ingress_accept_one(int fd, uint16_t port)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    uint8_t out[MQTT_MAX_PACKET_SIZE + 8];
    mqtt_packet_t pkt;
    mesh_ingress_client_t *client;
    int client_fd;
    int len;

    client_fd = plat_accept(fd, (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) {
        LOG_WRN("mesh ingress accept: %d", errno);
        return;
    }
    configure_client_socket(client_fd);
    LOG_INF("New mesh ingress connection fd=%d port=%u", client_fd, port);

    {
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        (void)plat_setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    if (recv_mqtt_packet(client_fd, &pkt) < 0 ||
        (pkt.type_flags & 0xf0) != MQTT_CONNECT) {
        plat_close(client_fd);
        return;
    }
    len = packet_build_connack(0, CONNACK_ACCEPTED, out, sizeof(out));
    if (len <= 0 || broker_send_all(client_fd, out, (size_t)len) < 0) {
        plat_close(client_fd);
        return;
    }
    client = mesh_client_alloc(client_fd);
    if (!client) {
        reject_server_unavailable(client_fd, "No free mesh ingress slots");
    }
}

static void mesh_ingress_handle_client(mesh_ingress_client_t *client)
{
    uint8_t out[MQTT_MAX_PACKET_SIZE + 8];
    mqtt_packet_t pkt;
    int len;

    if (!client || !client->in_use) {
        return;
    }
    if (recv_mqtt_packet(client->fd, &pkt) < 0) {
        mesh_client_close(client);
        return;
    }

    uint8_t type = pkt.type_flags & 0xf0;

    if (type == MQTT_PUBLISH) {
        mqtt_publish_t pub;

        if (packet_parse_publish(&pkt, &pub) == 0) {
            if (pub.qos == 2) {
                int slot = mesh_qos2_find(client, pub.packet_id);

                if (slot < 0) {
                    slot = mesh_qos2_alloc(client);
                    if (slot >= 0) {
                        client->qos2_in[slot].packet_id = pub.packet_id;
                        client->qos2_in[slot].pub = pub;
                        client->qos2_in[slot].published = 0;
                        client->qos2_in[slot].in_use = 1;
                    } else {
                        LOG_WRN("mesh ingress QoS2-in table full, closing id=%u",
                                pub.packet_id);
                        mesh_client_close(client);
                        return;
                    }
                }
                len = packet_build_pubrec(pub.packet_id, out, sizeof(out));
                if (len > 0) {
                    (void)broker_send_all(client->fd, out, (size_t)len);
                }
            } else {
                mesh_publish_from_ingress(&pub);
            }
            if (pub.qos == 1) {
                len = packet_build_puback(pub.packet_id, out, sizeof(out));
                if (len > 0) {
                    (void)broker_send_all(client->fd, out, (size_t)len);
                }
            }
        }
    } else if (type == MQTT_SUBSCRIBE) {
        uint16_t packet_id = 0;
        char topics[1][MQTT_TOPIC_MAX];
        uint8_t qos[1];
        uint8_t count = 0;
        uint8_t granted = 0x80;

        if (packet_parse_subscribe(&pkt, &packet_id, topics, qos, &count, 1) == 0 &&
            count > 0) {
            if (mesh_client_subscribe(client, topics[0], qos[0]) == 0) {
                granted = qos[0] & 0x03;
            }
        }
        len = packet_build_suback(packet_id, &granted, 1, out, sizeof(out));
        if (len > 0) {
            (void)broker_send_all(client->fd, out, (size_t)len);
        }
    } else if (type == MQTT_UNSUBSCRIBE) {
        uint16_t packet_id = 0;
        char topics[1][MQTT_TOPIC_MAX];
        uint8_t count = 0;

        if (packet_parse_unsubscribe(&pkt, &packet_id, topics, &count, 1) == 0 &&
            count > 0) {
            (void)mesh_client_unsubscribe(client, topics[0]);
        }
        len = packet_build_unsuback(packet_id, out, sizeof(out));
        if (len > 0) {
            (void)broker_send_all(client->fd, out, (size_t)len);
        }
    } else if (type == MQTT_PUBACK) {
        mesh_inflight_ack(client, packet_id_from_ack(&pkt));
    } else if (type == MQTT_PUBREC) {
        mesh_inflight_pubrec(client, packet_id_from_ack(&pkt));
    } else if (type == MQTT_PUBREL) {
        uint16_t packet_id = packet_id_from_ack(&pkt);
        int slot = mesh_qos2_find(client, packet_id);

        if (slot >= 0) {
            if (!client->qos2_in[slot].published) {
                mesh_publish_from_ingress(&client->qos2_in[slot].pub);
                client->qos2_in[slot].published = 1;
            }
            client->qos2_in[slot].in_use = 0;
        }
        len = packet_build_pubcomp(packet_id, out, sizeof(out));
        if (len > 0) {
            (void)broker_send_all(client->fd, out, (size_t)len);
        }
    } else if (type == MQTT_PUBCOMP) {
        mesh_inflight_ack(client, packet_id_from_ack(&pkt));
    } else if (type == MQTT_PINGREQ) {
        len = packet_build_pingresp(out, sizeof(out));
        if (len > 0) {
            (void)broker_send_all(client->fd, out, (size_t)len);
        }
    } else if (type == MQTT_DISCONNECT) {
        mesh_client_close(client);
    } else {
        mesh_client_close(client);
    }
}

void broker_mesh_ingress_publish_remote(const mqtt_publish_t *pub)
{
    uint8_t out[MQTT_MAX_PACKET_SIZE + 8];

    if (!pub) {
        return;
    }
    for (int i = 0; i < MESH_INGRESS_MAX_CLIENTS; i++) {
        mesh_ingress_client_t *client = &mesh_clients[i];

        if (!client->in_use) {
            continue;
        }
        for (int j = 0; j < MESH_INGRESS_SUBS_PER_CLIENT; j++) {
            mqtt_publish_t deliver;
            int len;

            if (!client->sub_in_use[j] ||
                !mqtt_topic_match(client->subs[j], pub->topic)) {
                continue;
            }
            deliver = *pub;
            deliver.qos = pub->qos < client->qos[j] ? pub->qos : client->qos[j];
            if (deliver.qos > 0) {
                if (++client->next_packet_id == 0) {
                    ++client->next_packet_id;
                }
                deliver.packet_id = client->next_packet_id;
            }
            len = packet_build_publish(&deliver, out, sizeof(out));
            if (len <= 0 ||
                broker_send_all(client->fd, out, (size_t)len) < 0) {
                mesh_client_close(client);
                break;
            }
            if (deliver.qos > 0) {
                mesh_inflight_store(client, deliver.packet_id, out,
                                    (uint16_t)len, deliver.qos);
            }
            break;
        }
    }
}

#ifndef __ZEPHYR__
static int mesh_ingress_pollfds(struct pollfd *fds, int max, int base)
{
    int n = 0;

    for (int i = 0; i < MESH_INGRESS_MAX_CLIENTS && base + n < max; i++) {
        if (mesh_clients[i].in_use) {
            fds[base + n].fd = mesh_clients[i].fd;
            fds[base + n].events = POLLIN;
            fds[base + n].revents = 0;
            n++;
        }
    }
    return n;
}
#endif

#ifdef __ZEPHYR__
static int mesh_ingress_zpollfds(struct zsock_pollfd *fds, int max, int base)
{
    int n = 0;

    for (int i = 0; i < MESH_INGRESS_MAX_CLIENTS && base + n < max; i++) {
        if (mesh_clients[i].in_use) {
            fds[base + n].fd = mesh_clients[i].fd;
            fds[base + n].events = ZSOCK_POLLIN;
            fds[base + n].revents = 0;
            n++;
        }
    }
    return n;
}
#endif

static void mesh_ingress_handle_ready_fd(int ready_fd)
{
    for (int i = 0; i < MESH_INGRESS_MAX_CLIENTS; i++) {
        if (mesh_clients[i].in_use && mesh_clients[i].fd == ready_fd) {
            mesh_ingress_handle_client(&mesh_clients[i]);
            return;
        }
    }
}

int broker_start_mesh_ingress_listener(uint16_t port)
{
    if (port == 0 || port == listen_port) {
        return -EINVAL;
    }
    if (extra_listener_started) {
        return 0;
    }
    extra_listen_fd = open_listener(port);
    if (extra_listen_fd < 0) {
        return extra_listen_fd;
    }
    extra_listen_port = port;
    extra_listener_started = 1;
    return 0;
}

void broker_run(void)
{
    if (!extra_listener_started) {
        while (1) {
            if (listen_fd >= 0) {
                broker_accept_one(listen_fd, listen_port);
            } else {
#ifdef __ZEPHYR__
                k_sleep(K_MSEC(100));
#else
                (void)poll(NULL, 0, 100);
#endif
            }
        }
    }

    while (1) {
#ifdef __ZEPHYR__
        struct zsock_pollfd fds[2 + MESH_INGRESS_MAX_CLIENTS] = {
            { .fd = listen_fd, .events = ZSOCK_POLLIN },
            { .fd = extra_listen_fd, .events = ZSOCK_POLLIN },
        };
        int nfds = 2 + mesh_ingress_zpollfds(fds, 2 + MESH_INGRESS_MAX_CLIENTS, 2);
        int rc = zsock_poll(fds, nfds, 1000);
#else
        struct pollfd fds[2 + MESH_INGRESS_MAX_CLIENTS] = {
            { .fd = listen_fd, .events = POLLIN },
            { .fd = extra_listen_fd, .events = POLLIN },
        };
        int nfds = 2 + mesh_ingress_pollfds(fds, 2 + MESH_INGRESS_MAX_CLIENTS, 2);
        int rc = poll(fds, nfds, 1000);
#endif

        if (rc < 0) {
            LOG_WRN("poll: %d", errno);
            continue;
        }
        if (rc == 0) {
            mesh_ingress_retry_all();
            continue;
        }
        if (listen_fd >= 0 && fds[0].revents) {
            broker_accept_one(listen_fd, listen_port);
        }
        if (fds[1].revents) {
            mesh_ingress_accept_one(extra_listen_fd, extra_listen_port);
        }
        for (int i = 2; i < nfds; i++) {
            if (fds[i].revents) {
                mesh_ingress_handle_ready_fd(fds[i].fd);
            }
        }
        mesh_ingress_retry_all();
    }
}
