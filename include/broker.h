#ifndef BROKER_H
#define BROKER_H

#define MQTT_BROKER_PORT   1883
#define MQTT_MAX_CLIENTS   8

int  broker_init(void);
void broker_run(void);   /* does not return */

#endif /* BROKER_H */
