#ifndef BROKER_H
#define BROKER_H

#include <stdint.h>

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT   1883
#endif

#ifndef MQTT_MAX_CLIENTS
#define MQTT_MAX_CLIENTS   8
#endif

#ifndef MQTT_ADMISSION_MAX_CLIENTS
#define MQTT_ADMISSION_MAX_CLIENTS MQTT_MAX_CLIENTS
#endif

typedef void (*broker_activity_cb_t)(void *ctx);

/*
 * Public broker lifecycle for standalone and embedded use.
 *
 * The embedder is responsible for bringing up the network interface before
 * broker_init(). On Zephyr this usually means connecting WiFi or Ethernet and
 * waiting for an assigned IP address. broker_init() initializes broker-owned
 * state and opens the listening socket; it returns 0 on success or a negative
 * error code on failure. Do not call broker_run() after a failed init.
 *
 * broker_run() enters the accept loop and does not return during normal
 * operation. It owns client allocation and per-client thread startup. When
 * CONFIG_MQTT_P2P_DYNAMIC is enabled, dynamic P2P routing is started by the
 * broker path; embedders do not need a separate P2P init call.
 *
 * broker_set_bind_host() optionally restricts the MQTT listener to one IPv4
 * address before broker_init(). Passing NULL or an empty string restores the
 * default INADDR_ANY behavior.
 *
 * broker_set_admission_limit() optionally caps the number of local MQTT clients
 * accepted by this broker. New CONNECT attempts beyond the limit receive
 * CONNACK SERVER_UNAVAILABLE so clients can fall back to another bridge broker.
 * Passing 0 restores the compile-time default.
 *
 * broker_set_activity_callback() registers a lightweight callback that is
 * invoked after a publish is accepted into the local topic router. It is meant
 * for embedders that need activity indicators and must not block.
 */
int  broker_set_bind_host(const char *host);
int  broker_set_admission_limit(uint16_t max_clients);
void broker_set_activity_callback(broker_activity_cb_t cb, void *ctx);
void broker_notify_activity(void);
int  broker_init(void);
void broker_run(void);

#endif /* BROKER_H */
