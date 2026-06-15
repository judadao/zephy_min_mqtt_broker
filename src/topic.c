#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "topic.h"
#include "client.h"

LOG_MODULE_REGISTER(mqtt_topic, LOG_LEVEL_DBG);

typedef struct {
    struct client *client;
    char           filter[MQTT_TOPIC_MAX];
    uint8_t        qos;
    uint8_t        in_use;
} sub_entry_t;

typedef struct {
    char    topic[MQTT_TOPIC_MAX];
    uint8_t payload[MQTT_PAYLOAD_MAX];
    uint16_t payload_len;
    uint8_t  qos;
    uint8_t  in_use;
} retain_entry_t;

static sub_entry_t    subs[TOPIC_MAX_SUBS];
static retain_entry_t retains[TOPIC_MAX_SUBS];
static K_MUTEX_DEFINE(topic_lock);

void topic_init(void)
{
    memset(subs,    0, sizeof(subs));
    memset(retains, 0, sizeof(retains));
}

/* return 1 if topic matches filter (supports + and #) */
static int topic_match(const char *filter, const char *topic)
{
    const char *f = filter;
    const char *t = topic;

    while (*f && *t) {
        if (*f == '#') {
            return 1; /* # matches everything remaining */
        }
        if (*f == '+') {
            /* skip one level in topic */
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

    /* handle trailing '#' */
    if (*f == '#') {
        return 1;
    }
    return (*f == '\0' && *t == '\0');
}

int topic_subscribe(struct client *c, const char *filter, uint8_t qos)
{
    k_mutex_lock(&topic_lock, K_FOREVER);

    /* update qos if already subscribed */
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (subs[i].in_use && subs[i].client == c &&
            strcmp(subs[i].filter, filter) == 0) {
            subs[i].qos = qos;
            k_mutex_unlock(&topic_lock);
            return 0;
        }
    }

    /* find free slot */
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (!subs[i].in_use) {
            subs[i].client = c;
            strncpy(subs[i].filter, filter, sizeof(subs[i].filter) - 1);
            subs[i].qos    = qos;
            subs[i].in_use = 1;
            k_mutex_unlock(&topic_lock);

            /* deliver any matching retained message */
            for (int j = 0; j < TOPIC_MAX_SUBS; j++) {
                if (retains[j].in_use &&
                    topic_match(filter, retains[j].topic)) {
                    mqtt_publish_t pub = {0};
                    strncpy(pub.topic, retains[j].topic, sizeof(pub.topic) - 1);
                    memcpy(pub.payload, retains[j].payload, retains[j].payload_len);
                    pub.payload_len = retains[j].payload_len;
                    pub.qos         = qos < retains[j].qos ? qos : retains[j].qos;
                    pub.retain      = 1;
                    uint8_t buf[MQTT_MAX_PACKET_SIZE + 8];
                    int len = packet_build_publish(&pub, buf, sizeof(buf));
                    if (len > 0) {
                        client_send(c, buf, (size_t)len);
                    }
                }
            }
            return 0;
        }
    }

    k_mutex_unlock(&topic_lock);
    return -1; /* table full */
}

int topic_unsubscribe(struct client *c, const char *filter)
{
    k_mutex_lock(&topic_lock, K_FOREVER);
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (subs[i].in_use && subs[i].client == c &&
            strcmp(subs[i].filter, filter) == 0) {
            subs[i].in_use = 0;
            break;
        }
    }
    k_mutex_unlock(&topic_lock);
    return 0;
}

void topic_unsubscribe_all(struct client *c)
{
    k_mutex_lock(&topic_lock, K_FOREVER);
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (subs[i].in_use && subs[i].client == c) {
            subs[i].in_use = 0;
        }
    }
    k_mutex_unlock(&topic_lock);
}

int topic_publish(const mqtt_publish_t *pub)
{
    /* store/clear retained message */
    if (pub->retain) {
        k_mutex_lock(&topic_lock, K_FOREVER);
        int found = -1;
        for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
            if (retains[i].in_use &&
                strcmp(retains[i].topic, pub->topic) == 0) {
                found = i;
                break;
            }
        }
        if (pub->payload_len == 0) {
            /* empty payload clears retain */
            if (found >= 0) {
                retains[found].in_use = 0;
            }
        } else {
            int slot = (found >= 0) ? found : -1;
            if (slot < 0) {
                for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
                    if (!retains[i].in_use) {
                        slot = i;
                        break;
                    }
                }
            }
            if (slot >= 0) {
                strncpy(retains[slot].topic, pub->topic,
                        sizeof(retains[slot].topic) - 1);
                memcpy(retains[slot].payload, pub->payload, pub->payload_len);
                retains[slot].payload_len = pub->payload_len;
                retains[slot].qos         = pub->qos;
                retains[slot].in_use      = 1;
            }
        }
        k_mutex_unlock(&topic_lock);
    }

    /* fan-out to matching subscribers */
    k_mutex_lock(&topic_lock, K_FOREVER);
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (!subs[i].in_use) {
            continue;
        }
        if (!topic_match(subs[i].filter, pub->topic)) {
            continue;
        }

        mqtt_publish_t out = *pub;
        out.retain      = 0; /* never forward retain flag to subscribers */
        out.qos         = out.qos < subs[i].qos ? out.qos : subs[i].qos;
        out.packet_id   = 0; /* TODO: assign from client->next_packet_id for QoS 1 */

        uint8_t buf[MQTT_MAX_PACKET_SIZE + 8];
        int     len = packet_build_publish(&out, buf, sizeof(buf));
        if (len > 0) {
            client_send(subs[i].client, buf, (size_t)len);
        }
    }
    k_mutex_unlock(&topic_lock);
    return 0;
}
