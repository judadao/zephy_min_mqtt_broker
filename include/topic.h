#ifndef TOPIC_H
#define TOPIC_H

#include <stdint.h>
#include "packet.h"
#include "broker.h"

/* forward declaration — include client.h in .c files */
struct client;

#define TOPIC_MAX_SUBS  (MQTT_MAX_CLIENTS * 8)

typedef struct {
    char    filter[MQTT_TOPIC_MAX];
    char    client_id[MQTT_CLIENT_ID_MAX];
    uint8_t qos;
} sub_snapshot_t;

typedef struct {
    char     topic[MQTT_TOPIC_MAX];
    uint16_t payload_len;
    uint8_t  qos;
} retain_snapshot_t;

int topic_get_sub_snapshots(sub_snapshot_t *out, int max);
int topic_get_retain_snapshots(retain_snapshot_t *out, int max);

void topic_init(void);
int  topic_subscribe(struct client *c, const char *filter, uint8_t qos);
int  topic_unsubscribe(struct client *c, const char *filter);
void topic_unsubscribe_all(struct client *c);
int  topic_publish(const mqtt_publish_t *pub);

#endif /* TOPIC_H */
