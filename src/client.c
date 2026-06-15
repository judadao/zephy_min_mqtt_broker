#include "platform/platform.h"
#include <string.h>
#include <errno.h>

#include "client.h"
#include "broker.h"
#include "packet.h"
#include "topic.h"
#include "session.h"

LOG_MODULE_REGISTER(mqtt_client, LOG_LEVEL_DBG);

static client_t clients[MQTT_MAX_CLIENTS];

#ifdef __ZEPHYR__
static K_THREAD_STACK_ARRAY_DEFINE(client_stacks, MQTT_MAX_CLIENTS, CLIENT_STACK_SIZE);
#endif

PLAT_MUTEX_DEFINE(pool_lock);

/* ---------- internal helpers ---------- */

static void handle_connect(client_t *c, const mqtt_packet_t *pkt);
static void handle_publish(client_t *c, const mqtt_packet_t *pkt);
static void handle_subscribe(client_t *c, const mqtt_packet_t *pkt);
static void handle_unsubscribe(client_t *c, const mqtt_packet_t *pkt);
static void handle_pingreq(client_t *c);
static void handle_disconnect(client_t *c);

static int recv_packet(client_t *c, mqtt_packet_t *out);

/* ---------- pool ---------- */

int client_get_snapshots(client_snapshot_t *out, int max)
{
    plat_mutex_lock(&pool_lock);
    int n = 0;
    for (int i = 0; i < MQTT_MAX_CLIENTS && n < max; i++) {
        if (clients[i].state == CLIENT_STATE_CONNECTED) {
            out[n].slot         = clients[i].slot;
            out[n].keepalive    = clients[i].keepalive;
            out[n].last_seen_ms = clients[i].last_seen_ms;
            strncpy(out[n].client_id, clients[i].client_id,
                    MQTT_CLIENT_ID_MAX - 1);
            n++;
        }
    }
    plat_mutex_unlock(&pool_lock);
    return n;
}

void client_pool_init(void)
{
    memset(clients, 0, sizeof(clients));
    for (int i = 0; i < MQTT_MAX_CLIENTS; i++) {
        clients[i].slot  = (uint8_t)i;
        clients[i].state = CLIENT_STATE_FREE;
        clients[i].fd    = -1;
    }
}

int client_alloc(int fd)
{
    plat_mutex_lock(&pool_lock);

    client_t *c = NULL;
    for (int i = 0; i < MQTT_MAX_CLIENTS; i++) {
        if (clients[i].state == CLIENT_STATE_FREE) {
            c = &clients[i];
            break;
        }
    }

    if (!c) {
        plat_mutex_unlock(&pool_lock);
        return -1;
    }

    memset(c, 0, sizeof(*c));
    c->slot  = (uint8_t)(c - clients);
    c->fd    = fd;
    c->state = CLIENT_STATE_CONNECTED;

#ifdef __ZEPHYR__
    k_thread_create(&c->thread,
                    client_stacks[c->slot],
                    K_THREAD_STACK_SIZEOF(client_stacks[c->slot]),
                    client_thread_fn,
                    c, NULL, NULL,
                    5, 0, K_NO_WAIT);
#else
    plat_thread_spawn(c);
#endif

    plat_mutex_unlock(&pool_lock);
    return c->slot;
}

void client_free(client_t *c)
{
    if (c->fd >= 0) {
        plat_close(c->fd);
        c->fd = -1;
    }
    /* publish will message on unexpected disconnect */
    if (c->has_will) {
        topic_publish(&c->will);
        c->has_will = 0;
    }
    topic_unsubscribe_all(c);
    if (c->clean_session) {
        session_delete(c->client_id);
    }
    c->state = CLIENT_STATE_FREE;
    LOG_INF("client[%d] freed (id=%s)", c->slot, c->client_id);
}

/* ---------- thread ---------- */

void client_thread_fn(void *p1, void *p2, void *p3)
{
    client_t   *c = (client_t *)p1;
    mqtt_packet_t pkt;

    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_DBG("client[%d] thread started", c->slot);

    while (c->state == CLIENT_STATE_CONNECTED) {
        int rc = recv_packet(c, &pkt);
        if (rc < 0) {
            if (errno == EAGAIN || errno == ETIMEDOUT) {
                if (c->keepalive > 0) {
                    int64_t idle_ms = plat_uptime_ms() - c->last_seen_ms;
                    if (idle_ms > (int64_t)c->keepalive * 1500) {
                        LOG_WRN("client[%d] keepalive timeout (id=%s)", c->slot, c->client_id);
                        break;
                    }
                }
                client_inflight_retry(c);
                continue;
            }
            break;
        }

        c->last_seen_ms = plat_uptime_ms();

        uint8_t type = pkt.type_flags & 0xF0;

        switch (type) {
        case MQTT_CONNECT:     handle_connect(c, &pkt);     break;
        case MQTT_PUBLISH:     handle_publish(c, &pkt);     break;
        case MQTT_SUBSCRIBE:   handle_subscribe(c, &pkt);   break;
        case MQTT_UNSUBSCRIBE: handle_unsubscribe(c, &pkt); break;
        case MQTT_PINGREQ:     handle_pingreq(c);           break;
        case MQTT_DISCONNECT:  handle_disconnect(c);        break;
        case MQTT_PUBACK:
            if (pkt.buf_len >= 2) {
                uint16_t ack_id = (uint16_t)((pkt.buf[0] << 8) | pkt.buf[1]);
                client_inflight_ack(c, ack_id);
            }
            break;
        default:
            LOG_WRN("client[%d] unknown packet type 0x%02x", c->slot, type);
            break;
        }
    }

    client_free(c);
}

/* ---------- packet I/O ---------- */

int client_send(client_t *c, const uint8_t *buf, size_t len)
{
    ssize_t sent = plat_send(c->fd, buf, len, 0);
    if (sent < 0 || (size_t)sent != len) {
        LOG_WRN("client[%d] send error: %d", c->slot, errno);
        c->state = CLIENT_STATE_DISCONNECTING;
        return -1;
    }
    return 0;
}

void client_disconnect(client_t *c)
{
    c->state = CLIENT_STATE_DISCONNECTING;
}

/* ---------- QoS-1 in-flight queue ---------- */

void client_inflight_store(client_t *c, uint16_t id, const uint8_t *buf, uint16_t len)
{
    for (int i = 0; i < CLIENT_INFLIGHT_MAX; i++) {
        if (!c->inflight[i].in_use) {
            c->inflight[i].packet_id  = id;
            c->inflight[i].len        = len;
            c->inflight[i].sent_at_ms = plat_uptime_ms();
            c->inflight[i].in_use     = 1;
            memcpy(c->inflight[i].buf, buf, len);
            return;
        }
    }
    LOG_WRN("client[%d] inflight table full, dropping QoS-1 id=%u", c->slot, id);
}

void client_inflight_ack(client_t *c, uint16_t id)
{
    for (int i = 0; i < CLIENT_INFLIGHT_MAX; i++) {
        if (c->inflight[i].in_use && c->inflight[i].packet_id == id) {
            c->inflight[i].in_use = 0;
            LOG_DBG("client[%d] inflight ack id=%u", c->slot, id);
            return;
        }
    }
    LOG_WRN("client[%d] PUBACK for unknown id=%u", c->slot, id);
}

void client_inflight_retry(client_t *c)
{
    int64_t now = plat_uptime_ms();
    for (int i = 0; i < CLIENT_INFLIGHT_MAX; i++) {
        if (!c->inflight[i].in_use) {
            continue;
        }
        if (now - c->inflight[i].sent_at_ms < CLIENT_INFLIGHT_RETRY_MS) {
            continue;
        }
        c->inflight[i].buf[0] |= 0x08; /* set DUP flag */
        c->inflight[i].sent_at_ms = now;
        LOG_DBG("client[%d] retransmit QoS-1 id=%u", c->slot, c->inflight[i].packet_id);
        client_send(c, c->inflight[i].buf, c->inflight[i].len);
    }
}

/* recv exactly n bytes from fd */
static int recv_exact(int fd, uint8_t *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = plat_recv(fd, buf + done, n - done, 0);
        if (r <= 0) {
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

/* read one complete MQTT packet from wire into *out */
static int recv_packet(client_t *c, mqtt_packet_t *out)
{
    uint8_t header[5];

    if (recv_exact(c->fd, header, 1) < 0) {
        return -1;
    }
    out->type_flags = header[0];

    uint32_t rlen  = 0;
    size_t   rbytes = 0;
    uint8_t  byte;
    uint32_t multiplier = 1;

    do {
        if (recv_exact(c->fd, &byte, 1) < 0) {
            return -1;
        }
        rbytes++;
        rlen += (byte & 0x7F) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128) {
            LOG_WRN("client[%d] malformed remaining-length", c->slot);
            return -1;
        }
    } while (byte & 0x80);

    out->remaining_len = rlen;

    if (rlen > MQTT_MAX_PACKET_SIZE) {
        LOG_WRN("client[%d] packet too large: %u", c->slot, rlen);
        return -1;
    }

    if (rlen > 0 && recv_exact(c->fd, out->buf, rlen) < 0) {
        return -1;
    }
    out->buf_len = rlen;

    ARG_UNUSED(rbytes);
    return 0;
}

/* ---------- packet handlers ---------- */

static void handle_connect(client_t *c, const mqtt_packet_t *pkt)
{
    mqtt_connect_t conn;
    uint8_t        resp[4];
    int            len;

    if (packet_parse_connect(pkt, &conn) < 0) {
        LOG_WRN("client[%d] CONNECT parse error", c->slot);
        c->state = CLIENT_STATE_DISCONNECTING;
        return;
    }

#if defined(CONFIG_MQTT_AUTH_ENABLED)
    if (!conn.has_username || !conn.has_password ||
        strcmp(conn.username, CONFIG_MQTT_AUTH_USERNAME) != 0 ||
        strcmp(conn.password, CONFIG_MQTT_AUTH_PASSWORD) != 0) {
        LOG_WRN("client[%d] auth failed (id=%s user=%s)",
                c->slot, conn.client_id,
                conn.has_username ? conn.username : "(none)");
        uint8_t rej[4];
        int rlen = packet_build_connack(0, CONNACK_BAD_CREDENTIALS, rej, sizeof(rej));
        client_send(c, rej, (size_t)rlen);
        c->state = CLIENT_STATE_DISCONNECTING;
        return;
    }
#endif

    strncpy(c->client_id, conn.client_id, sizeof(c->client_id) - 1);
    c->clean_session = conn.clean_session;
    c->keepalive     = conn.keepalive;
    c->has_will      = conn.has_will;
    if (conn.has_will) {
        strncpy(c->will.topic, conn.will_topic, sizeof(c->will.topic) - 1);
        memcpy(c->will.payload, conn.will_payload, conn.will_payload_len);
        c->will.payload_len = conn.will_payload_len;
        c->will.qos         = conn.will_qos;
        c->will.retain      = conn.will_retain;
    }

    uint8_t session_present = 0;
    if (!conn.clean_session) {
        session_t *s = session_find(conn.client_id);
        if (s) {
            session_present = 1;
        } else {
            session_create(conn.client_id);
        }
    }

    len = packet_build_connack(session_present, CONNACK_ACCEPTED, resp, sizeof(resp));
    client_send(c, resp, (size_t)len);

    /* 5-second tick for keepalive enforcement and QoS-1 in-flight retry */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    plat_setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    LOG_INF("client[%d] CONNECT id=%s clean=%d keepalive=%us",
            c->slot, c->client_id, c->clean_session, c->keepalive);
}

static void handle_publish(client_t *c, const mqtt_packet_t *pkt)
{
    mqtt_publish_t pub;
    uint8_t        ack[4];

    if (packet_parse_publish(pkt, &pub) < 0) {
        LOG_WRN("client[%d] PUBLISH parse error", c->slot);
        return;
    }

    LOG_DBG("client[%d] PUBLISH topic=%s qos=%d", c->slot, pub.topic, pub.qos);

    topic_publish(&pub);

    if (pub.qos == 1) {
        int len = packet_build_puback(pub.packet_id, ack, sizeof(ack));
        client_send(c, ack, (size_t)len);
    }
}

static void handle_subscribe(client_t *c, const mqtt_packet_t *pkt)
{
    char    topics[8][MQTT_TOPIC_MAX];
    uint8_t qos[8];
    uint8_t count     = 0;
    uint16_t packet_id = 0;
    uint8_t  resp[32];

    if (packet_parse_subscribe(pkt, &packet_id, topics, qos, &count, 8) < 0) {
        return;
    }

    uint8_t return_codes[8];
    for (int i = 0; i < count; i++) {
        int rc = topic_subscribe(c, topics[i], qos[i]);
        return_codes[i] = (rc == 0) ? qos[i] : 0x80;
        LOG_DBG("client[%d] SUBSCRIBE %s qos=%d rc=%d", c->slot, topics[i], qos[i], rc);
    }

    int len = packet_build_suback(packet_id, return_codes, count, resp, sizeof(resp));
    client_send(c, resp, (size_t)len);
}

static void handle_unsubscribe(client_t *c, const mqtt_packet_t *pkt)
{
    char    topics[8][MQTT_TOPIC_MAX];
    uint8_t count     = 0;
    uint16_t packet_id = 0;
    uint8_t  resp[4];

    if (packet_parse_unsubscribe(pkt, &packet_id, topics, &count, 8) < 0) {
        return;
    }

    for (int i = 0; i < count; i++) {
        topic_unsubscribe(c, topics[i]);
    }

    int len = packet_build_unsuback(packet_id, resp, sizeof(resp));
    client_send(c, resp, (size_t)len);
}

static void handle_pingreq(client_t *c)
{
    uint8_t resp[2];
    int     len = packet_build_pingresp(resp, sizeof(resp));
    client_send(c, resp, (size_t)len);
}

static void handle_disconnect(client_t *c)
{
    LOG_INF("client[%d] DISCONNECT id=%s", c->slot, c->client_id);
    c->has_will = 0;
    c->state    = CLIENT_STATE_DISCONNECTING;
}
