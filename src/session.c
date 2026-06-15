#include <string.h>
#include "platform/platform.h"
#include "session.h"
#include "client.h"

LOG_MODULE_REGISTER(mqtt_session, LOG_LEVEL_DBG);

static session_t    sessions[SESSION_MAX];
PLAT_MUTEX_DEFINE(session_lock);

void session_init(void)
{
    memset(sessions, 0, sizeof(sessions));
}

session_t *session_find(const char *client_id)
{
    plat_mutex_lock(&session_lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (sessions[i].in_use &&
            strcmp(sessions[i].client_id, client_id) == 0) {
            plat_mutex_unlock(&session_lock);
            return &sessions[i];
        }
    }
    plat_mutex_unlock(&session_lock);
    return NULL;
}

session_t *session_create(const char *client_id)
{
    plat_mutex_lock(&session_lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (!sessions[i].in_use) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            strncpy(sessions[i].client_id, client_id,
                    sizeof(sessions[i].client_id) - 1);
            sessions[i].in_use = 1;
            plat_mutex_unlock(&session_lock);
            LOG_DBG("session created for %s", client_id);
            return &sessions[i];
        }
    }
    plat_mutex_unlock(&session_lock);
    return NULL;
}

void session_delete(const char *client_id)
{
    plat_mutex_lock(&session_lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (sessions[i].in_use &&
            strcmp(sessions[i].client_id, client_id) == 0) {
            sessions[i].in_use = 0;
            LOG_DBG("session deleted for %s", client_id);
            break;
        }
    }
    plat_mutex_unlock(&session_lock);
}

void session_save_subs(session_t *s,
                       const char (*filters)[MQTT_TOPIC_MAX],
                       const uint8_t *qos, uint8_t count)
{
    uint8_t n = (count < SESSION_SUB_MAX) ? count : SESSION_SUB_MAX;
    for (uint8_t i = 0; i < n; i++) {
        strncpy(s->sub_filters[i], filters[i], MQTT_TOPIC_MAX - 1);
        s->sub_qos[i] = qos[i];
    }
    s->sub_count = n;
    s->offline   = 1;
}

/* caller must hold session_lock when iterating sessions (see session_offline_publish) */
int session_enqueue(session_t *s, const mqtt_publish_t *pub, uint16_t pkt_id)
{
    for (int i = 0; i < SESSION_QUEUE_MAX; i++) {
        if (!s->queue[i].in_use) {
            strncpy(s->queue[i].topic, pub->topic, MQTT_TOPIC_MAX - 1);
            uint16_t plen = pub->payload_len < MQTT_PAYLOAD_MAX
                            ? pub->payload_len : MQTT_PAYLOAD_MAX;
            memcpy(s->queue[i].payload, pub->payload, plen);
            s->queue[i].payload_len = plen;
            s->queue[i].qos        = pub->qos;
            s->queue[i].packet_id  = pkt_id;
            s->queue[i].in_use     = 1;
            LOG_DBG("enqueued for %s: %s", s->client_id, pub->topic);
            return 0;
        }
    }
    LOG_WRN("session queue full for %s, dropping %s", s->client_id, pub->topic);
    return -1;
}

void session_drain(session_t *s, struct client *c)
{
    for (int i = 0; i < SESSION_QUEUE_MAX; i++) {
        if (!s->queue[i].in_use) {
            continue;
        }
        mqtt_publish_t pub = {0};
        strncpy(pub.topic, s->queue[i].topic, MQTT_TOPIC_MAX - 1);
        memcpy(pub.payload, s->queue[i].payload, s->queue[i].payload_len);
        pub.payload_len = s->queue[i].payload_len;
        pub.qos         = s->queue[i].qos;
        pub.packet_id   = s->queue[i].packet_id;

        uint8_t buf[MQTT_MAX_PACKET_SIZE + 8];
        int     len = packet_build_publish(&pub, buf, sizeof(buf));
        if (len > 0) {
            client_send(c, buf, (size_t)len);
            if (pub.qos > 0) {
                client_inflight_store(c, pub.packet_id, buf, (uint16_t)len, pub.qos);
            }
        }
        s->queue[i].in_use = 0;
        LOG_DBG("drained for %s: %s", s->client_id, pub.topic);
    }
}

int session_offline_publish(const mqtt_publish_t *pub,
                             int (*match_fn)(const char *, const char *))
{
    if (pub->qos == 0) {
        return 0; /* QoS 0 is fire-and-forget; never queue for offline clients */
    }
    int count = 0;
    plat_mutex_lock(&session_lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (!sessions[i].in_use || !sessions[i].offline) {
            continue;
        }
        for (int j = 0; j < sessions[i].sub_count; j++) {
            if (!match_fn(sessions[i].sub_filters[j], pub->topic)) {
                continue;
            }
            uint8_t qos = pub->qos < sessions[i].sub_qos[j]
                          ? pub->qos : sessions[i].sub_qos[j];
            if (qos > 0) {
                if (sessions[i].next_packet_id == 0) {
                    sessions[i].next_packet_id = 1;
                }
                uint16_t pkt_id = sessions[i].next_packet_id++;
                mqtt_publish_t q = *pub;
                q.qos       = qos;
                q.packet_id = pkt_id;
                session_enqueue(&sessions[i], &q, pkt_id);
                count++;
            }
            break; /* one enqueue per session even if multiple filters match */
        }
    }
    plat_mutex_unlock(&session_lock);
    return count;
}
