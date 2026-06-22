#include <string.h>
#include "platform/platform.h"
#include "topic.h"
#include "client.h"
#include "session.h"
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
#include "p2p.h"
#include "p2p_shard.h"
#endif

LOG_MODULE_REGISTER(mqtt_topic, LOG_LEVEL_DBG);

typedef struct {
    struct client *client;
    char           filter[MQTT_TOPIC_MAX];
    uint8_t        qos;
    uint8_t        in_use;
    int            exact_next;
    int            wildcard_next;
    uint8_t        is_exact;
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
static int exact_heads[TOPIC_MAX_SUBS];
static int wildcard_head;
PLAT_MUTEX_DEFINE(topic_lock);

static uint32_t topic_hash(const char *s)
{
    uint32_t hash = 2166136261u;

    while (*s) {
        hash ^= (uint8_t)*s++;
        hash *= 16777619u;
    }
    return hash;
}

static int filter_is_exact_local(const char *filter)
{
    return strpbrk(filter, "+#") == NULL;
}

static void exact_list_insert_locked(int slot)
{
    uint32_t bucket = topic_hash(subs[slot].filter) % TOPIC_MAX_SUBS;

    subs[slot].exact_next = exact_heads[bucket];
    exact_heads[bucket] = slot;
}

static void wildcard_list_insert_locked(int slot)
{
    subs[slot].wildcard_next = wildcard_head;
    wildcard_head = slot;
}

static void exact_list_remove_locked(int slot)
{
    uint32_t bucket = topic_hash(subs[slot].filter) % TOPIC_MAX_SUBS;
    int *link = &exact_heads[bucket];

    while (*link >= 0) {
        if (*link == slot) {
            *link = subs[slot].exact_next;
            return;
        }
        link = &subs[*link].exact_next;
    }
}

static void wildcard_list_remove_locked(int slot)
{
    int *link = &wildcard_head;

    while (*link >= 0) {
        if (*link == slot) {
            *link = subs[slot].wildcard_next;
            return;
        }
        link = &subs[*link].wildcard_next;
    }
}

#if defined(CONFIG_MQTT_P2P_DYNAMIC)
static int filter_stats_locked(const char *filter, uint8_t *max_qos)
{
    int count = 0;
    uint8_t qos = 0;

    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (subs[i].in_use && strcmp(subs[i].filter, filter) == 0) {
            count++;
            if (subs[i].qos > qos) {
                qos = subs[i].qos;
            }
        }
    }

    if (max_qos) {
        *max_qos = qos;
    }
    return count;
}
#endif

int topic_get_sub_snapshots(sub_snapshot_t *out, int max)
{
    int cursor = 0;

    return topic_get_sub_snapshots_from(out, max, &cursor);
}

int topic_get_sub_snapshots_from(sub_snapshot_t *out, int max, int *cursor)
{
    plat_mutex_lock(&topic_lock);
    int start = cursor ? *cursor : 0;
    int n = 0;
    for (int i = start; i < TOPIC_MAX_SUBS && n < max; i++) {
        if (subs[i].in_use) {
            memset(&out[n], 0, sizeof(out[n]));
            strncpy(out[n].filter,    subs[i].filter,             MQTT_TOPIC_MAX - 1);
            strncpy(out[n].client_id, subs[i].client->client_id,  MQTT_CLIENT_ID_MAX - 1);
            out[n].qos = subs[i].qos;
            n++;
        }
        if (cursor) {
            *cursor = i + 1;
        }
    }
    if (cursor && *cursor > TOPIC_MAX_SUBS) {
        *cursor = TOPIC_MAX_SUBS;
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
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        exact_heads[i] = -1;
    }
    wildcard_head = -1;
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
    /* MQTT 3.1.1 §4.7.2: wildcards must not match topics starting with '$' */
    if (topic[0] == '$' && (filter[0] == '+' || filter[0] == '#')) {
        return 0;
    }

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
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    uint8_t old_max_qos = 0;
    uint8_t new_max_qos;
    int old_count = 0;
    int notify_sub = 0;
#endif

    plat_mutex_lock(&topic_lock);

    /* Single pass: find existing slot for this client+filter, first free slot,
     * and (for P2P) count all subscriptions to this filter with their max QoS. */
    int free_slot = -1;
    int found_slot = -1;
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    uint8_t found_slot_qos = 0;
#endif
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (!subs[i].in_use) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
        if (strcmp(subs[i].filter, filter) == 0) {
            old_count++;
            if (subs[i].qos > old_max_qos) old_max_qos = subs[i].qos;
            if (subs[i].client == c) {
                found_slot = i;
                found_slot_qos = subs[i].qos;
            }
        }
#else
        if (subs[i].client == c && strcmp(subs[i].filter, filter) == 0) {
            found_slot = i;
            break;
        }
#endif
    }

    if (found_slot >= 0) {
        subs[found_slot].qos = qos;
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
        if (qos >= old_max_qos) {
            new_max_qos = qos;
        } else if (found_slot_qos < old_max_qos) {
            new_max_qos = old_max_qos; /* slot wasn't the max; max unchanged */
        } else {
            (void)filter_stats_locked(filter, &new_max_qos); /* was max, lowered */
        }
        notify_sub = (new_max_qos != old_max_qos);
#endif
        plat_mutex_unlock(&topic_lock);
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
        if (notify_sub) {
            p2p_local_subscribe(filter, new_max_qos);
        }
#endif
        return 0;
    }

    if (free_slot >= 0) {
        int i = free_slot;
        subs[i].client   = c;
        strncpy(subs[i].filter, filter, sizeof(subs[i].filter) - 1);
        subs[i].qos      = qos;
        subs[i].in_use   = 1;
        subs[i].is_exact = (uint8_t)filter_is_exact_local(filter);
        if (subs[i].is_exact) {
            exact_list_insert_locked(i);
        } else {
            wildcard_list_insert_locked(i);
        }
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
        new_max_qos = (qos > old_max_qos) ? qos : old_max_qos;
        notify_sub = (old_count == 0 || new_max_qos != old_max_qos);
#endif
        plat_mutex_unlock(&topic_lock);
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
        if (notify_sub) {
            p2p_local_subscribe(filter, new_max_qos);
        }
#endif
        return 0;
    }

    plat_mutex_unlock(&topic_lock);
    return -1; /* table full */
}

int topic_unsubscribe(struct client *c, const char *filter)
{
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    uint8_t old_max_qos = 0;
    uint8_t new_max_qos = 0;
    uint8_t removed_qos = 0;
    int old_count = 0;
    int notify_unsub = 0;
    int notify_sub = 0;
#endif

    plat_mutex_lock(&topic_lock);

    /* Single pass: compute filter stats and locate the slot to remove. */
    int target = -1;
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (!subs[i].in_use || strcmp(subs[i].filter, filter) != 0) {
            continue;
        }
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
        old_count++;
        if (subs[i].qos > old_max_qos) old_max_qos = subs[i].qos;
#endif
        if (subs[i].client == c) {
            target = i;
#if !defined(CONFIG_MQTT_P2P_DYNAMIC)
            break;
#endif
        }
    }

    if (target >= 0) {
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
        removed_qos = subs[target].qos;
#endif
        if (subs[target].is_exact) {
            exact_list_remove_locked(target);
        } else {
            wildcard_list_remove_locked(target);
        }
        subs[target].in_use = 0;
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
        notify_unsub = (old_count == 1);
        if (!notify_unsub) {
            if (removed_qos < old_max_qos) {
                new_max_qos = old_max_qos; /* max slot not removed */
            } else {
                (void)filter_stats_locked(filter, &new_max_qos);
                notify_sub = (new_max_qos != old_max_qos);
            }
        }
#endif
    }
    plat_mutex_unlock(&topic_lock);
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    if (notify_unsub) {
        p2p_local_unsubscribe(filter);
    } else if (notify_sub) {
        p2p_local_subscribe(filter, new_max_qos);
    }
#endif
    return 0;
}

void topic_unsubscribe_all(struct client *c)
{
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    while (1) {
        char removed[MQTT_TOPIC_MAX] = {0};
        int found = 0;
        int old_count;
        int new_count;
        uint8_t old_max_qos = 0;
        uint8_t new_max_qos = 0;
        int notify_unsub = 0;
        int notify_sub = 0;

        plat_mutex_lock(&topic_lock);
        for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
            if (subs[i].in_use && subs[i].client == c) {
                strncpy(removed, subs[i].filter, sizeof(removed) - 1);
                old_count = filter_stats_locked(removed, &old_max_qos);
                if (subs[i].is_exact) {
                    exact_list_remove_locked(i);
                } else {
                    wildcard_list_remove_locked(i);
                }
                subs[i].in_use = 0;
                new_count = filter_stats_locked(removed, &new_max_qos);
                notify_unsub = (old_count > 0 && new_count == 0);
                notify_sub = (new_count > 0 && new_max_qos != old_max_qos);
                found = 1;
                break;
            }
        }
        plat_mutex_unlock(&topic_lock);

        if (!found) {
            break;
        }
        if (notify_unsub) {
            p2p_local_unsubscribe(removed);
        } else if (notify_sub) {
            p2p_local_subscribe(removed, new_max_qos);
        }
    }
#else
    plat_mutex_lock(&topic_lock);
    for (int i = 0; i < TOPIC_MAX_SUBS; i++) {
        if (subs[i].in_use && subs[i].client == c) {
            if (subs[i].is_exact) {
                exact_list_remove_locked(i);
            } else {
                wildcard_list_remove_locked(i);
            }
            subs[i].in_use = 0;
        }
    }
    plat_mutex_unlock(&topic_lock);
#endif
}

typedef struct {
    client_t *client;
    uint8_t qos;
    uint16_t packet_id;
} delivery_target_t;

static void add_qos_delivery_target_locked(delivery_target_t *targets,
                                           int *target_count,
                                           const mqtt_publish_t *pub,
                                           int idx)
{
    delivery_target_t *target;

    if (*target_count >= TOPIC_MAX_SUBS) {
        return;
    }

    target = &targets[*target_count];
    target->client = subs[idx].client;
    target->qos = pub->qos < subs[idx].qos ? pub->qos : subs[idx].qos;
    target->packet_id = 0;
    if (target->qos > 0) {
        if (++target->client->next_packet_id == 0) {
            ++target->client->next_packet_id; /* skip 0, invalid per spec */
        }
        target->packet_id = target->client->next_packet_id;
    }
    (*target_count)++;
}

static int topic_publish_internal(const mqtt_publish_t *pub, int propagate)
{
    /* One lock covers both the retain store update and fan-out. */
    plat_mutex_lock(&topic_lock);

    if (pub->retain) {
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
            } else {
                LOG_WRN("retain store full (%d slots), dropping topic=%s",
                        TOPIC_RETAIN_MAX, pub->topic);
            }
        }
    }

    /* fan-out to matching subscribers */
    uint32_t bucket = topic_hash(pub->topic) % TOPIC_MAX_SUBS;
    if (pub->qos == 0) {
        client_t *targets[TOPIC_MAX_SUBS];
        uint8_t shared_buf[MQTT_MAX_PACKET_SIZE + 8];
        mqtt_publish_t shared = *pub;
        int shared_len;

        shared.retain = 0;
        shared.packet_id = 0;
        shared_len = packet_build_publish(&shared, shared_buf, sizeof(shared_buf));
        if (shared_len > 0) {
            int target_count = 0;
            for (int idx = exact_heads[bucket]; idx >= 0; idx = subs[idx].exact_next) {
                if (!subs[idx].in_use || strcmp(subs[idx].filter, pub->topic) != 0) {
                    continue;
                }
                /* dedup: skip if client already queued */
                int dup = 0;
                for (int d = 0; d < target_count; d++) {
                    if (targets[d] == subs[idx].client) { dup = 1; break; }
                }
                if (!dup) targets[target_count++] = subs[idx].client;
            }
            for (int idx = wildcard_head; idx >= 0; idx = subs[idx].wildcard_next) {
                if (!subs[idx].in_use) {
                    continue;
                }
                if (!topic_match(subs[idx].filter, pub->topic)) {
                    continue;
                }
                /* dedup: skip if client already queued */
                int dup = 0;
                for (int d = 0; d < target_count; d++) {
                    if (targets[d] == subs[idx].client) { dup = 1; break; }
                }
                if (!dup) targets[target_count++] = subs[idx].client;
            }
            plat_mutex_unlock(&topic_lock);

            for (int i = 0; i < target_count; i++) {
                client_send(targets[i], shared_buf, (size_t)shared_len);
            }
        } else {
            plat_mutex_unlock(&topic_lock);
        }
    } else {
        client_t *seen[TOPIC_MAX_SUBS];
        delivery_target_t targets[TOPIC_MAX_SUBS];
        int seen_count = 0;
        int target_count = 0;

        for (int idx = exact_heads[bucket]; idx >= 0; idx = subs[idx].exact_next) {
            if (!subs[idx].in_use || strcmp(subs[idx].filter, pub->topic) != 0) {
                continue;
            }
            int dup = 0;
            for (int d = 0; d < seen_count; d++) {
                if (seen[d] == subs[idx].client) { dup = 1; break; }
            }
            if (!dup) {
                seen[seen_count++] = subs[idx].client;
                add_qos_delivery_target_locked(targets, &target_count, pub, idx);
            }
        }

        for (int idx = wildcard_head; idx >= 0; idx = subs[idx].wildcard_next) {
            if (!subs[idx].in_use || !topic_match(subs[idx].filter, pub->topic)) {
                continue;
            }
            int dup = 0;
            for (int d = 0; d < seen_count; d++) {
                if (seen[d] == subs[idx].client) { dup = 1; break; }
            }
            if (!dup) {
                seen[seen_count++] = subs[idx].client;
                add_qos_delivery_target_locked(targets, &target_count, pub, idx);
            }
        }
        plat_mutex_unlock(&topic_lock);

        for (int i = 0; i < target_count; i++) {
            mqtt_publish_t out = *pub;
            uint8_t buf[MQTT_MAX_PACKET_SIZE + 8];
            int len;

            out.retain = 0; /* never forward retain flag to subscribers */
            out.qos = targets[i].qos;
            out.packet_id = targets[i].packet_id;
            len = packet_build_publish(&out, buf, sizeof(buf));
            if (len > 0) {
                client_send(targets[i].client, buf, (size_t)len);
                if (out.qos > 0) {
                    client_inflight_store(targets[i].client, out.packet_id, buf,
                                          (uint16_t)len, out.qos);
                }
            }
        }
    }

    /* queue for offline persistent-session subscribers (QoS 0 skipped inside) */
    session_offline_publish(pub, topic_match);
#if defined(CONFIG_MQTT_P2P_DYNAMIC)
    if (propagate) {
#ifdef P2P_BENCH_TRACE
        LOG_INF("topic propagate publish topic=%s", pub->topic);
#endif
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
    struct {
        uint8_t  buf[MQTT_MAX_PACKET_SIZE + 8];
        int      len;
        uint8_t  qos;
        uint16_t packet_id;
    } pkts[TOPIC_RETAIN_MAX];
    int npkts = 0;

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
        if (pub.qos > 0) {
            if (++c->next_packet_id == 0) {
                ++c->next_packet_id;
            }
            pub.packet_id = c->next_packet_id;
        }
        pkts[npkts].len = packet_build_publish(&pub, pkts[npkts].buf,
                                               sizeof(pkts[npkts].buf));
        if (pkts[npkts].len > 0) {
            pkts[npkts].qos = pub.qos;
            pkts[npkts].packet_id = pub.packet_id;
            npkts++;
        }
    }
    plat_mutex_unlock(&topic_lock);

    for (int i = 0; i < npkts; i++) {
        client_send(c, pkts[i].buf, (size_t)pkts[i].len);
        if (pkts[i].qos > 0) {
            client_inflight_store(c, pkts[i].packet_id, pkts[i].buf,
                                  (uint16_t)pkts[i].len, pkts[i].qos);
        }
    }
}
