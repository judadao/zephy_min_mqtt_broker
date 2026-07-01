#include <string.h>
#ifndef __ZEPHYR__
#include <stdlib.h>
#endif
#include "platform/platform.h"
#include "session.h"
#include "client.h"

LOG_MODULE_REGISTER(mqtt_session, LOG_LEVEL_DBG);

static session_t *sessions;
PLAT_MUTEX_DEFINE(session_lock);

static session_t *session_pool_ensure(void)
{
    if (sessions) {
        return sessions;
    }
#ifdef __ZEPHYR__
    sessions = k_calloc(SESSION_MAX, sizeof(session_t));
#else
    sessions = calloc(SESSION_MAX, sizeof(session_t));
#endif
    return sessions;
}

void session_init(void)
{
    if (sessions) {
        memset(sessions, 0, SESSION_MAX * sizeof(session_t));
    }
}

session_t *session_find(const char *client_id)
{
    if (!sessions) {
        return NULL;
    }
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
    if (!session_pool_ensure()) {
        return NULL;
    }
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

session_t *session_find_or_create(const char *client_id, uint8_t *was_found)
{
    session_t *free_slot = NULL;

    if (was_found) {
        *was_found = 0;
    }
    if (!session_pool_ensure()) {
        return NULL;
    }
    plat_mutex_lock(&session_lock);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (sessions[i].in_use) {
            if (strcmp(sessions[i].client_id, client_id) == 0) {
                plat_mutex_unlock(&session_lock);
                if (was_found) {
                    *was_found = 1;
                }
                return &sessions[i];
            }
        } else if (!free_slot) {
            free_slot = &sessions[i];
        }
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        strncpy(free_slot->client_id, client_id, sizeof(free_slot->client_id) - 1);
        free_slot->in_use = 1;
        plat_mutex_unlock(&session_lock);
        LOG_DBG("session created for %s", client_id);
        return free_slot;
    }
    plat_mutex_unlock(&session_lock);
    return NULL;
}

void session_delete(const char *client_id)
{
    if (!sessions) {
        return;
    }
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
            s->queue[i].dup        = pub->dup;
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
        pub.dup         = s->queue[i].dup; /* propagate DUP for inflight retransmits */

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
    if (!sessions) {
        return 0;
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
