#ifndef BROKER_H
#define BROKER_H

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT   1883
#endif

#ifndef MQTT_MAX_CLIENTS
#define MQTT_MAX_CLIENTS   8
#endif

/*
 * Lifecycle:
 *   1. Bring up the network interface before calling broker_init().
 *   2. broker_init() returns 0 on success, negative errno-style value on error.
 *      Do not call broker_run() if broker_init() fails.
 *   3. broker_run() enters the main accept loop and never returns.
 *   4. If CONFIG_MQTT_P2P_DYNAMIC is defined, P2P routing is enabled
 *      automatically inside broker_run() — no extra init call is required.
 */
int  broker_init(void);
void broker_run(void);   /* does not return */

#endif /* BROKER_H */
