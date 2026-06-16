#include <string.h>
#include "platform/platform.h"
#include "topic.h"
#include "client.h"
#include "session.h"
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
#include "p2p.h"
#endif

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
static retain_entry_t retains[TOPIC_RETAIN_MAX];
PLAT_MUTEX_DEFINE(topic_lock);

int topic_get_sub_snapshots(sub_snapshot_t *out, int max)
{
    plat_mutex_lock(&topic_lock);
    int n = 0;
    for (int i = 0; i < TOPIC_MAX_SUBS && n < max; i++) {
        if (subs[i].in_use) {
            strncpy(out[n].filter,    subs[i].filter,             MQTT_TOPIC_MAX - 1);
            strncpy(out[n].client_id, subs[i].client->client_id,  MQTT_CLIENT_ID_MAX - 1);
            out[n].qos = subs[i].qos;
            n++;
        }
    }
    plat_mutex_unlock(&topic_lock);
    return n;
}

int topic_get_retain_snapshots(retain_snapshot_t *out, int max)
{
    plat_mutex_lock(&topic_lock);
    int n = 0;
    for (int i = 0; i < TOPIC_RETAIN_MAX && n < max; i++) {
        if (retains[i].in_use) {
            strncpy(out[n].topic, retains[i].topic, MQTT_TOPIC_MAX - 1);
            out[n].payload_len = retains[i].payload_len;
            out[n].qos         = retains[i].qos;
            n++;
        }
    }
    plat_mutex_unlock(&topic_lock);
    return n;
}

void topic_init(void)
{
    memset(subs,    0, sizeof(subs));
    memset(retains, 0, sizeof(retains));
}

/* return 1 if filter is a valid MQTT topic filter (MQTT 3.1.1 §4.7.1) */
int topic_filter_valid(const char *filter)
{
    if (!filter || filter[0] == '\0')
        return 0;

    for (const char *p = filter; *p; p++) {
        if (*p == '#') {
            /* '#' must be the last character */
            if (*(p + 1) != '\0')
                return 0;
            /* if not at start, must be preceded by '/' */
            if (p != filter && *(p - 1) != '/')
                return 0;
        } else if (*p == '+') {
            /* '+' must be preceded by '/' or be first character */
            if (p != filter && *(p - 1) != '/')
                return 0;
            /* '+' must be followed by '/' or end of string */
            if (*(p + 1) != '/' && *(p + 1) != '\0')
                return 0;
        }
    }
    return 1;
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
    /* MQTT 3.1.1 §4.7.1: "sport/tennis/#" matches "sport/tennis" */
    if (*f == '/' && *(f + 1) == '#' && *(f + 2) == '\0' && *t == '\0') {
        return 1;
    }
    return (*f == '\0' && *t == '\0');
}

int topic_subscribe(struct client *c, const char *filter, uint8_t qos)
{
    plat_mutex_lock(&topic_lock);

    /* update qos if already subscribed */
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (subs[i].in_use && subs[i].client == c &&
            strcmp(subs[i].filter, filter) == 0) {
            subs[i].qos = qos;
            plat_mutex_unlock(&topic_lock);
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
            p2p_local_subscribe(filter, qos);
#endif
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
            plat_mutex_unlock(&topic_lock);
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
            p2p_local_subscribe(filter, qos);
#endif
            return 0;
        }
    }

    plat_mutex_unlock(&topic_lock);
    return -1; /* table full */
}

int topic_unsubscribe(struct client *c, const char *filter)
{
    plat_mutex_lock(&topic_lock);
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (subs[i].in_use && subs[i].client == c &&
            strcmp(subs[i].filter, filter) == 0) {
            subs[i].in_use = 0;
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
            p2p_local_unsubscribe(filter);
#endif
            break;
        }
    }
    plat_mutex_unlock(&topic_lock);
    return 0;
}

void topic_unsubscribe_all(struct client *c)
{
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    while (1) {
        char removed[MQTT_TOPIC_MAX] = {0};
        int found = 0;

        plat_mutex_lock(&topic_lock);
        for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
            if (subs[i].in_use && subs[i].client == c) {
                strncpy(removed, subs[i].filter, sizeof(removed) - 1);
                subs[i].in_use = 0;
                found = 1;
                break;
            }
        }
        plat_mutex_unlock(&topic_lock);

        if (!found) {
            break;
        }
        p2p_local_unsubscribe(removed);
    }
#else
    plat_mutex_lock(&topic_lock);
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (subs[i].in_use && subs[i].client == c) {
            subs[i].in_use = 0;
        }
    }
    plat_mutex_unlock(&topic_lock);
#endif
}

static int topic_publish_internal(const mqtt_publish_t *pub, int propagate)
{
    /* store/clear retained message */
    if (pub->retain) {
        plat_mutex_lock(&topic_lock);
        int found = -1;
        for (int i = 0; i < TOPIC_RETAIN_MAX; i++) {
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
                for (int i = 0; i < TOPIC_RETAIN_MAX; i++) {
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
        plat_mutex_unlock(&topic_lock);
    }

    /* fan-out to matching subscribers */
    plat_mutex_lock(&topic_lock);
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (!subs[i].in_use) {
            continue;
        }
        if (!topic_match(subs[i].filter, pub->topic)) {
            continue;
        }

        mqtt_publish_t out = *pub;
        out.retain = 0; /* never forward retain flag to subscribers */
        out.qos    = out.qos < subs[i].qos ? out.qos : subs[i].qos;
        if (out.qos > 0) {
            if (++subs[i].client->next_packet_id == 0) {
                ++subs[i].client->next_packet_id; /* skip 0, invalid per spec */
            }
            out.packet_id = subs[i].client->next_packet_id;
        } else {
            out.packet_id = 0;
        }

        uint8_t buf[MQTT_MAX_PACKET_SIZE + 8];
        int     len = packet_build_publish(&out, buf, sizeof(buf));
        if (len > 0) {
            client_send(subs[i].client, buf, (size_t)len);
            if (out.qos > 0) {
                client_inflight_store(subs[i].client, out.packet_id,
                                      buf, (uint16_t)len, out.qos);
            }
        }
    }
    plat_mutex_unlock(&topic_lock);

    /* queue for offline persistent-session subscribers (QoS 0 skipped inside) */
    session_offline_publish(pub, topic_match);
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    if (propagate) {
        p2p_publish_from_local(pub);
    }
#else
    ARG_UNUSED(propagate);
#endif
    return 0;
}

int topic_publish(const mqtt_publish_t *pub)
{
    return topic_publish_internal(pub, 1);
}

int topic_publish_remote(const mqtt_publish_t *pub)
{
    return topic_publish_internal(pub, 0);
}

int topic_get_client_subs(struct client *c,
                           char out[][MQTT_TOPIC_MAX],
                           uint8_t *out_qos, uint8_t max)
{
    plat_mutex_lock(&topic_lock);
    int n = 0;
    for (int i = 0; i < TOPIC_MAX_SUBS && (uint8_t)n < max; i++) {
        if (subs[i].in_use && subs[i].client == c) {
            strncpy(out[n], subs[i].filter, MQTT_TOPIC_MAX - 1);
            out_qos[n] = subs[i].qos;
            n++;
        }
    }
    plat_mutex_unlock(&topic_lock);
    return n;
}

void topic_deliver_retained(struct client *c, const char *filter, uint8_t qos)
{
    plat_mutex_lock(&topic_lock);
    for (int j = 0; j < TOPIC_RETAIN_MAX; j++) {
        if (!retains[j].in_use || !topic_match(filter, retains[j].topic)) {
            continue;
        }
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
    plat_mutex_unlock(&topic_lock);
}
