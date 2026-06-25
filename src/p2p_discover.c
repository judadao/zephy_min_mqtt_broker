#include "platform/platform.h"
#include <string.h>

#if defined(__ZEPHYR__) && defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#ifndef __ZEPHYR__
#include <stdlib.h>
#include <time.h>
#endif

#include "p2p.h"

LOG_MODULE_REGISTER(mqtt_p2p_discover, LOG_LEVEL_INF);

#if !defined(CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY)
static int udp_fd = -1;

#ifdef __ZEPHYR__
#define P2P_STACK_SIZE 1024
static struct k_thread announce_thread;
static struct k_thread listen_thread;
static K_THREAD_STACK_DEFINE(announce_stack, P2P_STACK_SIZE);
static K_THREAD_STACK_DEFINE(listen_stack, P2P_STACK_SIZE);
#else
static pthread_t announce_thread;
static pthread_t listen_thread;
#endif
#endif

static void make_node_id(uint8_t out[P2P_NODE_ID_LEN])
{
#if defined(__ZEPHYR__) && defined(CONFIG_HWINFO)
    uint8_t device_id[32];
    ssize_t len = hwinfo_get_device_id(device_id, sizeof(device_id));

    if (len > 0) {
        uint32_t hash = 2166136261u;

        for (int i = 0; i < P2P_NODE_ID_LEN; i++) {
            uint8_t b = device_id[(size_t)i % (size_t)len];

            hash ^= b;
            hash *= 16777619u;
            hash ^= (uint8_t)i;
            hash *= 16777619u;
            out[i] = (uint8_t)(hash >> 24);
        }
        return;
    }
#endif

    uint32_t seed = (uint32_t)plat_uptime_ms();
#ifndef __ZEPHYR__
    seed ^= (uint32_t)getpid();
    seed ^= (uint32_t)time(NULL);
#endif
    for (int i = 0; i < P2P_NODE_ID_LEN; i++) {
        seed = seed * 1103515245u + 12345u;
        out[i] = (uint8_t)(seed >> 16);
    }
}

#if !defined(CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY)
static void p2p_announce_loop(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(P2P_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };

    while (1) {
        p2p_announce_t ann;
        p2p_election_build_announce(&ann);
        (void)plat_sendto(udp_fd, &ann, sizeof(ann), 0,
                          (struct sockaddr *)&dst, sizeof(dst));
#ifdef __ZEPHYR__
        k_sleep(K_MSEC(P2P_ANNOUNCE_MS));
#else
        struct timespec ts = {
            .tv_sec = P2P_ANNOUNCE_MS / 1000,
            .tv_nsec = (P2P_ANNOUNCE_MS % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
#endif
    }
}

static void p2p_listen_loop(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        p2p_announce_t ann;
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        ssize_t n = plat_recvfrom(udp_fd, &ann, sizeof(ann), 0,
                                  (struct sockaddr *)&src, &src_len);
        if (n == (ssize_t)sizeof(ann)) {
            p2p_election_update_peer(&ann, src.sin_addr.s_addr);
        }
    }
}

#ifndef __ZEPHYR__
static void *announce_main(void *arg)
{
    p2p_announce_loop(arg, NULL, NULL);
    return NULL;
}

static void *listen_main(void *arg)
{
    p2p_listen_loop(arg, NULL, NULL);
    return NULL;
}
#endif
#endif

void p2p_start(void)
{
    uint8_t node_id[P2P_NODE_ID_LEN];

#if defined(CONFIG_MQTT_P2P_STATIC_SEEDS_ONLY)
    make_node_id(node_id);
    p2p_election_init(node_id);
    p2p_peer_start();
    LOG_INF("P2P static seed-only broker mode enabled on TCP %d", P2P_PORT);
#else
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(P2P_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int opt = 1;

    udp_fd = plat_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_fd < 0) {
        LOG_ERR("P2P discovery socket failed: %d", errno);
        return;
    }

    plat_setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    plat_setsockopt(udp_fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    if (plat_bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("P2P discovery bind failed: %d", errno);
        plat_close(udp_fd);
        udp_fd = -1;
        return;
    }

    make_node_id(node_id);
    p2p_election_init(node_id);

#ifdef __ZEPHYR__
    k_thread_create(&announce_thread, announce_stack, K_THREAD_STACK_SIZEOF(announce_stack),
                    p2p_announce_loop, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
    k_thread_create(&listen_thread, listen_stack, K_THREAD_STACK_SIZEOF(listen_stack),
                    p2p_listen_loop, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
#else
    {
        int rc = pthread_create(&announce_thread, NULL, announce_main, NULL);
        if (rc == 0) {
            pthread_detach(announce_thread);
        } else {
            LOG_ERR("P2P discovery announce thread failed: %d", rc);
        }
        rc = pthread_create(&listen_thread, NULL, listen_main, NULL);
        if (rc == 0) {
            pthread_detach(listen_thread);
        } else {
            LOG_ERR("P2P discovery listen thread failed: %d", rc);
        }
    }
#endif
    p2p_peer_start();
    LOG_INF("P2P dynamic broker discovery enabled on UDP %d", P2P_DISCOVERY_PORT);
#endif
}
