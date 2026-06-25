#include "platform/platform.h"
#include <errno.h>
#include <string.h>
#ifndef __ZEPHYR__
#include <netinet/tcp.h>
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
static char bind_host[64];
static uint16_t admission_limit = MQTT_ADMISSION_MAX_CLIENTS;
static broker_activity_cb_t activity_cb;
static void *activity_ctx;

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

int broker_init(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(MQTT_BROKER_PORT),
    };
    const char *listen_host = bind_host[0] ? bind_host : "0.0.0.0";

    topic_init();
    session_init();
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    p2p_router_init();
#endif

    listen_fd = plat_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        LOG_ERR("socket: %d", errno);
        return -errno;
    }

    int opt = 1;
    plat_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind_host[0] && plat_inet_pton(AF_INET, bind_host, &addr.sin_addr) != 1) {
        LOG_ERR("invalid bind host: %s", bind_host);
        plat_close(listen_fd);
        listen_fd = -1;
        return -EINVAL;
    }

    if (plat_bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind: %d", errno);
        plat_close(listen_fd);
        listen_fd = -1;
        return -errno;
    }

    if (plat_listen(listen_fd, MQTT_MAX_CLIENTS) < 0) {
        LOG_ERR("listen: %d", errno);
        plat_close(listen_fd);
        listen_fd = -1;
        return -errno;
    }

    if (admission_limit > MQTT_MAX_CLIENTS) {
        admission_limit = MQTT_MAX_CLIENTS;
    }

    LOG_INF("Listening on %s:%d (max %d clients, admission %u)",
            listen_host, MQTT_BROKER_PORT, MQTT_MAX_CLIENTS, admission_limit);
    return 0;
}

void broker_run(void)
{
    while (1) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);

        int fd = plat_accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            LOG_WRN("accept: %d", errno);
            continue;
        }
        configure_client_socket(fd);

        LOG_INF("New TCP connection fd=%d", fd);

        if (admission_limit < MQTT_MAX_CLIENTS &&
            client_used_slots() >= admission_limit) {
            reject_server_unavailable(fd, "Admission limit reached");
            continue;
        }

        if (client_alloc(fd) < 0) {
            /* MQTT 3.1.1 §3.2.2.3: send SERVER_UNAVAILABLE then close */
            reject_server_unavailable(fd, "No free client slots");
        }
    }
}
