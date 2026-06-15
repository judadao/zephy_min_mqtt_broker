#include "platform/platform.h"
#include <errno.h>

#include "broker.h"
#include "client.h"
#include "topic.h"
#include "session.h"

LOG_MODULE_REGISTER(mqtt_broker, LOG_LEVEL_INF);

static int listen_fd = -1;

int broker_init(void)
{
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(MQTT_BROKER_PORT),
    };

    topic_init();
    session_init();

    listen_fd = plat_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        LOG_ERR("socket: %d", errno);
        return -errno;
    }

    int opt = 1;
    plat_setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (plat_bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("bind: %d", errno);
        plat_close(listen_fd);
        return -errno;
    }

    if (plat_listen(listen_fd, MQTT_MAX_CLIENTS) < 0) {
        LOG_ERR("listen: %d", errno);
        plat_close(listen_fd);
        return -errno;
    }

    LOG_INF("Listening on port %d (max %d clients)", MQTT_BROKER_PORT, MQTT_MAX_CLIENTS);
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

        LOG_INF("New TCP connection fd=%d", fd);

        if (client_alloc(fd) < 0) {
            LOG_WRN("No free client slots, dropping fd=%d", fd);
            plat_close(fd);
        }
    }
}
