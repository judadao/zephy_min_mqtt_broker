#include "platform/platform.h"
#include "broker.h"
#include "client.h"

LOG_MODULE_REGISTER(mqtt_main, LOG_LEVEL_INF);

int main(void)
{
    LOG_INF("MQTT minimal broker starting");

    /* TODO: init WiFi and wait for DHCP before calling broker_init() (Zephyr/ESP32 only) */

    client_pool_init();

    if (broker_init() != 0) {
        LOG_ERR("broker_init failed");
        return -1;
    }

    broker_run(); /* does not return */
    return 0;
}
