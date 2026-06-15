#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include "packet.h"

#define SESSION_MAX  MQTT_MAX_CLIENTS

typedef struct {
    char    client_id[MQTT_CLIENT_ID_MAX];
    uint8_t in_use;
    /* extend here for persistent queued messages (QoS 1) */
} session_t;

void       session_init(void);
session_t *session_find(const char *client_id);
session_t *session_create(const char *client_id);
void       session_delete(const char *client_id);

#endif /* SESSION_H */
