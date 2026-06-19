#include "platform/platform.h"
#include "broker.h"
#include "client.h"
#include "wifi.h"
#if defined(CONFIG_MQTT_HTTP_DASHBOARD) && !defined(__ZEPHYR__)
#include "http.h"
#endif
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
#include "p2p.h"
#endif

LOG_MODULE_REGISTER(mqtt_main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("MQTT minimal broker starting");

#ifdef __ZEPHYR__
    if (wifi_connect() != 0) {
        LOG_ERR("WiFi init failed, halting");
        return -1;
    }
#endif

    client_pool_init();

    if (broker_init() != 0) {
        LOG_ERR("broker_init failed");
        return -1;
    }

#if defined(CONFIG_MQTT_HTTP_DASHBOARD) && !defined(__ZEPHYR__)
    if (http_server_start(8080) != 0) {
        LOG_ERR("HTTP dashboard start failed");
    }
#endif

#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    p2p_start();
#endif

    broker_run(); /* does not return */
    return 0;
}
