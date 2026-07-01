#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "broker.h"
#include "client.h"
#include "p2p.h"

static void stop_primary_listener(int sig)
{
    (void)sig;
    (void)broker_stop_mqtt_listener();
}

static void start_primary_listener(int sig)
{
    (void)sig;
    (void)broker_start_mqtt_listener();
}

int main(int argc, char **argv)
{
    uint16_t mqtt_port = 1883;
    uint16_t fallback_port = 1884;

    if (argc > 1) {
        mqtt_port = (uint16_t)atoi(argv[1]);
    }
    if (argc > 2) {
        fallback_port = (uint16_t)atoi(argv[2]);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, stop_primary_listener);
    signal(SIGUSR2, start_primary_listener);

    client_pool_init();
    if (broker_set_bind_host("127.0.0.1") != 0 ||
        broker_set_listen_port(mqtt_port) != 0 ||
        broker_init() != 0 ||
        broker_start_mesh_ingress_listener(fallback_port) != 0) {
        fprintf(stderr, "failed to start broker listeners\n");
        return 1;
    }

#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    p2p_start();
#endif

    broker_run();
    return 0;
}
