#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include "packet.h"
#include "broker.h"

#define SESSION_MAX       MQTT_MAX_CLIENTS
/* Holds offline messages + QoS1 inflights saved on disconnect (CLIENT_INFLIGHT_MAX=4) */
#define SESSION_QUEUE_MAX 8
#define SESSION_SUB_MAX   4   /* max persisted subscriptions per session    */

typedef struct {
    char     topic[MQTT_TOPIC_MAX];
    uint8_t  payload[MQTT_PAYLOAD_MAX];
    uint16_t payload_len;
    uint8_t  qos;
    uint16_t packet_id;
    uint8_t  dup;    /* 1 if retransmit (was inflight when client disconnected) */
    uint8_t  in_use;
} session_msg_t;

typedef struct {
    char          client_id[MQTT_CLIENT_ID_MAX];
    uint8_t       in_use;
    uint8_t       offline;          /* 1 while client is disconnected */
    uint16_t      next_packet_id;   /* for assigning ids to queued msgs */
    char          sub_filters[SESSION_SUB_MAX][MQTT_TOPIC_MAX];
    uint8_t       sub_qos[SESSION_SUB_MAX];
    uint8_t       sub_count;
    session_msg_t queue[SESSION_QUEUE_MAX];
} session_t;

struct client; /* forward declaration — avoids pulling in client.h */

void       session_init(void);
session_t *session_find(const char *client_id);
session_t *session_create(const char *client_id);
/* Find existing session or create a new one in one lock+scan.
 * Sets *was_found (if non-NULL) to 1 if found, 0 if created. */
session_t *session_find_or_create(const char *client_id, uint8_t *was_found);
void       session_delete(const char *client_id);

/* save the client's current subscription list into the session */
void session_save_subs(session_t *s,
                       const char (*filters)[MQTT_TOPIC_MAX],
                       const uint8_t *qos, uint8_t count);

/* enqueue one QoS-1 message for later delivery (caller holds session_lock) */
int  session_enqueue(session_t *s, const mqtt_publish_t *pub, uint16_t pkt_id);

/* deliver all queued messages to a freshly reconnected client */
void session_drain(session_t *s, struct client *c);

/* iterate offline sessions; enqueue pub for each matching filter (QoS>=1 only) */
int  session_offline_publish(const mqtt_publish_t *pub,
                              int (*match_fn)(const char *, const char *));

#endif /* SESSION_H */
