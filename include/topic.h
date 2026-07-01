#ifndef TOPIC_H
#define TOPIC_H

#include <stdint.h>
#include "packet.h"
#include "broker.h"

/* forward declaration — include client.h in .c files */
struct client;

#ifndef TOPIC_MAX_SUBS
#define TOPIC_MAX_SUBS   (MQTT_MAX_CLIENTS * 8)
#endif
#define TOPIC_RETAIN_MAX 8

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

/*
 * Snapshot APIs for dashboards and embedders.
 *
 * The caller supplies an output array and capacity. Functions return the number
 * of entries written. topic_get_sub_snapshots_from() advances *cursor so a
 * caller can page through the subscription table without exposing internal
 * storage. Returned strings are copies and remain valid after the call.
 */
int topic_get_sub_snapshots(sub_snapshot_t *out, int max);
int topic_get_sub_snapshots_from(sub_snapshot_t *out, int max, int *cursor);
int topic_get_retain_snapshots(retain_snapshot_t *out, int max);

/*
 * Topic table lifecycle.
 *
 * topic_init() clears subscriptions, retained messages, and routing indexes.
 * Call it once during broker initialization before accepting clients. The
 * remaining functions are thread-safe internally and are normally called from
 * client/session/P2P code, not directly from application tasks.
 */
void topic_init(void);

/*
 * Validate an MQTT topic filter for SUBSCRIBE. Returns 1 for a valid filter and
 * 0 for invalid wildcard placement or an empty filter.
 */
int  topic_filter_valid(const char *filter);

/*
 * Add, update, or remove local subscriptions for a connected client. QoS must
 * already be validated as 0, 1, or 2. topic_subscribe() returns 0 on success or
 * -1 if the fixed subscription table is full.
 */
int  topic_subscribe(struct client *c, const char *filter, uint8_t qos);
int  topic_unsubscribe(struct client *c, const char *filter);
void topic_unsubscribe_all(struct client *c);
int  topic_filter_subscriber_count(const char *filter);

/*
 * Publish into the local topic router. topic_publish() is used for local MQTT
 * clients and retained-store updates; topic_publish_remote() is used for P2P
 * ingress and avoids re-advertising the message back to peers.
 */
int  topic_publish(const mqtt_publish_t *pub);
int  topic_publish_remote(const mqtt_publish_t *pub);

/*
 * Deliver retained messages matching filter to a subscriber after SUBACK has
 * been sent. The delivered QoS is min(requested QoS, retained publish QoS).
 */
void topic_deliver_retained(struct client *c, const char *filter, uint8_t qos);

/* Write up to max of client c's subscriptions into out/out_qos; returns count. */
int  topic_get_client_subs(struct client *c,
                            char out[][MQTT_TOPIC_MAX],
                            uint8_t *out_qos, uint8_t max);

#endif /* TOPIC_H */
