#ifndef BROKER_H
#define BROKER_H

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT   1883
#endif

#ifndef MQTT_MAX_CLIENTS
#define MQTT_MAX_CLIENTS   8
#endif

int  broker_init(void);
void broker_run(void);   /* does not return */

#endif /* BROKER_H */
