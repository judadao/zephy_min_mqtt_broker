#ifdef __ZEPHYR__

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "wifi.h"

LOG_MODULE_REGISTER(mqtt_wifi, LOG_LEVEL_INF);

#define WIFI_CONNECT_TIMEOUT_S  30
#define WIFI_IP_TIMEOUT_S       30
#define WIFI_RECONNECT_DELAY_S   5

static K_SEM_DEFINE(sem_wifi_connected, 0, 1);
static K_SEM_DEFINE(sem_ip_obtained,    0, 1);

static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static void reconnect_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(reconnect_work, reconnect_work_fn);

/* forward declarations for functions defined below */
static int connect_wifi(struct net_if *iface);
static int setup_ip(struct net_if *iface);

static void reconnect_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        k_work_schedule(&reconnect_work, K_SECONDS(WIFI_RECONNECT_DELAY_S));
        return;
    }
    LOG_INF("WiFi reconnecting...");
    k_sem_reset(&sem_wifi_connected);
    k_sem_reset(&sem_ip_obtained);
    int rc = connect_wifi(iface);
    if (rc == 0) {
        rc = setup_ip(iface);
    }
    if (rc != 0) {
        LOG_WRN("reconnect failed (%d), retry in %ds", rc, WIFI_RECONNECT_DELAY_S);
        k_work_schedule(&reconnect_work, K_SECONDS(WIFI_RECONNECT_DELAY_S));
    } else {
        LOG_INF("WiFi reconnected");
    }
}

static void on_wifi_event(struct net_mgmt_event_callback *cb,
                          uint32_t event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *st = (const struct wifi_status *)cb->info;
        if (st && st->status != 0) {
            LOG_ERR("WiFi association failed (status=%d)", st->status);
            return;
        }
        LOG_INF("WiFi associated");
        k_sem_give(&sem_wifi_connected);
    } else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        LOG_WRN("WiFi disconnected — reconnecting in %ds", WIFI_RECONNECT_DELAY_S);
        k_work_schedule(&reconnect_work, K_SECONDS(WIFI_RECONNECT_DELAY_S));
    }
}

static void on_ipv4_event(struct net_mgmt_event_callback *cb,
                          uint32_t event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);

    if (event == NET_EVENT_IPV4_ADDR_ADD) {
        LOG_INF("IPv4 address obtained");
        k_sem_give(&sem_ip_obtained);
    }
}

static int connect_wifi(struct net_if *iface)
{
    struct wifi_connect_req_params params = {
        .ssid        = (const uint8_t *)CONFIG_MQTT_WIFI_SSID,
        .ssid_length = (uint8_t)strlen(CONFIG_MQTT_WIFI_SSID),
        .psk         = (uint8_t *)CONFIG_MQTT_WIFI_PASSWORD,
        .psk_length  = (uint8_t)strlen(CONFIG_MQTT_WIFI_PASSWORD),
        .channel     = WIFI_CHANNEL_ANY,
        .security    = WIFI_SECURITY_TYPE_PSK,
        .mfp         = WIFI_MFP_OPTIONAL,
        .timeout     = SYS_FOREVER_MS,
    };

    int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    if (rc) {
        LOG_ERR("WiFi connect request failed (%d)", rc);
        return rc;
    }

    if (k_sem_take(&sem_wifi_connected, K_SECONDS(WIFI_CONNECT_TIMEOUT_S))) {
        LOG_ERR("WiFi connect timeout after %ds", WIFI_CONNECT_TIMEOUT_S);
        return -ETIMEDOUT;
    }
    return 0;
}

static int setup_ip(struct net_if *iface)
{
#if defined(CONFIG_MQTT_WIFI_DHCP)
    LOG_INF("Starting DHCP...");
    net_dhcpv4_start(iface);
    if (k_sem_take(&sem_ip_obtained, K_SECONDS(WIFI_IP_TIMEOUT_S))) {
        LOG_ERR("DHCP timeout after %ds", WIFI_IP_TIMEOUT_S);
        return -ETIMEDOUT;
    }
#else /* CONFIG_MQTT_WIFI_STATIC */
    struct in_addr addr, mask, gw;

    if (net_addr_pton(AF_INET, CONFIG_MQTT_WIFI_STATIC_ADDR, &addr) < 0 ||
        net_addr_pton(AF_INET, CONFIG_MQTT_WIFI_STATIC_NETMASK, &mask) < 0 ||
        net_addr_pton(AF_INET, CONFIG_MQTT_WIFI_STATIC_GW, &gw) < 0) {
        LOG_ERR("Invalid static IP config");
        return -EINVAL;
    }

    net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
    net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
    net_if_ipv4_set_gw(iface, &gw);

    LOG_INF("Static IP: %s", CONFIG_MQTT_WIFI_STATIC_ADDR);
#endif
    return 0;
}

int wifi_connect(void)
{
    struct net_if *iface = net_if_get_default();

    if (!iface) {
        LOG_ERR("No network interface");
        return -ENODEV;
    }

    net_mgmt_init_event_callback(&wifi_cb, on_wifi_event,
        NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);

    net_mgmt_init_event_callback(&ipv4_cb, on_ipv4_event,
        NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&ipv4_cb);

    LOG_INF("Connecting to \"%s\"...", CONFIG_MQTT_WIFI_SSID);

    int rc = connect_wifi(iface);
    if (rc) {
        return rc;
    }

    return setup_ip(iface);
}

#endif /* __ZEPHYR__ */
