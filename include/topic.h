#ifndef TOPIC_H
#define TOPIC_H

#include <stdint.h>
#include "packet.h"
#include "broker.h"

/* forward declaration — include client.h in .c files */
struct client;

#define TOPIC_MAX_SUBS  (MQTT_MAX_CLIENTS * 8)

void topic_init(void);
int  topic_subscribe(struct client *c, const char *filter, uint8_t qos);
int  topic_unsubscribe(struct client *c, const char *filter);
void topic_unsubscribe_all(struct client *c);
int  topic_publish(const mqtt_publish_t *pub);

#endif /* TOPIC_H */
