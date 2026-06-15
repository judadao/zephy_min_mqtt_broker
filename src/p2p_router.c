#include "platform/platform.h"
#include <string.h>

#include "p2p.h"

LOG_MODULE_REGISTER(mqtt_p2p_router, LOG_LEVEL_INF);

#define P2P_REMOTE_SUB_MAX 32

typedef struct {
    uint8_t owner_id[P2P_NODE_ID_LEN];
    char filter[MQTT_TOPIC_MAX];
    uint8_t qos;
    uint8_t in_use;
} remote_sub_t;

static remote_sub_t remote_subs[P2P_REMOTE_SUB_MAX];
PLAT_MUTEX_DEFINE(router_lock);

static int id_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, P2P_NODE_ID_LEN) == 0;
}

static int topic_match(const char *filter, const char *topic)
{
    const char *f = filter;
    const char *t = topic;

    while (*f && *t) {
        if (*f == '#') {
            return 1;
        }
        if (*f == '+') {
            while (*t && *t != '/') {
                t++;
            }
            f++;
        } else {
            if (*f != *t) {
                return 0;
            }
            f++;
            t++;
        }
    }
    if (*f == '#') {
        return 1;
    }
    return *f == '\0' && *t == '\0';
}

void p2p_router_remote_subscribe(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                 const char *filter, uint8_t qos)
{
    int slot = -1;

    plat_mutex_lock(&router_lock);
    for (int i = 0; i < P2P_REMOTE_SUB_MAX; i++) {
        if (remote_subs[i].in_use &&
            id_equal(remote_subs[i].owner_id, owner_id) &&
            strcmp(remote_subs[i].filter, filter) == 0) {
            slot = i;
            break;
        }
        if (!remote_subs[i].in_use && slot < 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        memcpy(remote_subs[slot].owner_id, owner_id, P2P_NODE_ID_LEN);
        strncpy(remote_subs[slot].filter, filter, sizeof(remote_subs[slot].filter) - 1);
        remote_subs[slot].qos = qos;
        remote_subs[slot].in_use = 1;
    }
    plat_mutex_unlock(&router_lock);
}

void p2p_router_remote_unsubscribe(const uint8_t owner_id[P2P_NODE_ID_LEN],
                                   const char *filter)
{
    plat_mutex_lock(&router_lock);
    for (int i = 0; i < P2P_REMOTE_SUB_MAX; i++) {
        if (remote_subs[i].in_use &&
            id_equal(remote_subs[i].owner_id, owner_id) &&
            strcmp(remote_subs[i].filter, filter) == 0) {
            remote_subs[i].in_use = 0;
        }
    }
    plat_mutex_unlock(&router_lock);
}

void p2p_router_remove_node(const uint8_t owner_id[P2P_NODE_ID_LEN])
{
    plat_mutex_lock(&router_lock);
    for (int i = 0; i < P2P_REMOTE_SUB_MAX; i++) {
        if (remote_subs[i].in_use &&
            id_equal(remote_subs[i].owner_id, owner_id)) {
            remote_subs[i].in_use = 0;
        }
    }
    plat_mutex_unlock(&router_lock);
}

int p2p_router_topic_has_remote_match(const uint8_t node_id[P2P_NODE_ID_LEN],
                                      const char *topic)
{
    int match = 0;

    plat_mutex_lock(&router_lock);
    for (int i = 0; i < P2P_REMOTE_SUB_MAX; i++) {
        if (remote_subs[i].in_use &&
            id_equal(remote_subs[i].owner_id, node_id) &&
            topic_match(remote_subs[i].filter, topic)) {
            match = 1;
            break;
        }
    }
    plat_mutex_unlock(&router_lock);
    return match;
}

void p2p_router_publish(const p2p_publish_msg_t *msg,
                        const uint8_t *exclude_node_id)
{
    p2p_send_publish_from_router(msg, exclude_node_id);
}
