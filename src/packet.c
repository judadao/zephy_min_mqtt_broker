#include <string.h>
#include "platform/platform.h"
#include "packet.h"

LOG_MODULE_REGISTER(mqtt_packet, LOG_LEVEL_DBG);

/* ---------- variable-length encoding ---------- */

int packet_decode_remaining_len(const uint8_t *buf, size_t buf_len,
                                 uint32_t *out_len, size_t *out_bytes)
{
    uint32_t value      = 0;
    uint32_t multiplier = 1;
    size_t   i          = 0;

    if (!buf || !out_len || !out_bytes) {
        return -1;
    }

    do {
        if (i >= buf_len || i >= 4) {
            return -1;
        }
        value += (buf[i] & 0x7F) * multiplier;
        multiplier *= 128;
        i++;
    } while (buf[i - 1] & 0x80);

    *out_len   = value;
    *out_bytes = i;
    return 0;
}

int packet_encode_remaining_len(uint32_t len, uint8_t *out, size_t *out_bytes)
{
    size_t i = 0;

    if (!out || !out_bytes) {
        return -1;
    }

    do {
        out[i] = len % 128;
        len   /= 128;
        if (len > 0) {
            out[i] |= 0x80;
        }
        i++;
    } while (len > 0 && i < 4);

    *out_bytes = i;
    return (len > 0) ? -1 : 0;
}

/* ---------- read helpers (big-endian uint16) ---------- */

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int read_str(const uint8_t *buf, size_t buf_len, size_t *pos,
                    char *out, size_t out_cap)
{
    if (*pos + 2 > buf_len) {
        return -1;
    }
    uint16_t len = read_u16(buf + *pos);
    *pos += 2;
    if (*pos + len > buf_len || len >= out_cap) {
        return -1;
    }
    /* MQTT 3.1.1 §4.7.3: strings MUST NOT include null (U+0000) */
    if (memchr(buf + *pos, '\0', len)) {
        return -1;
    }
    memcpy(out, buf + *pos, len);
    out[len] = '\0';
    *pos += len;
    return 0;
}

/* ---------- write helpers ---------- */

static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static size_t write_str(uint8_t *buf, size_t pos, const char *s, uint16_t len)
{
    write_u16(buf + pos, len);
    memcpy(buf + pos + 2, s, len);
    return pos + 2 + len;
}

static int bounded_strlen(const char *s, size_t cap, size_t *out_len)
{
    size_t len = 0;

    while (len < cap && s[len] != '\0') {
        len++;
    }
    if (len == cap) {
        return -1;
    }
    *out_len = len;
    return 0;
}

/* ---------- parse incoming ---------- */

int packet_parse_connect(const mqtt_packet_t *pkt, mqtt_connect_t *out)
{
    const uint8_t *b   = pkt->buf;
    size_t         len = pkt->buf_len;
    size_t         pos = 0;

    /* protocol name: MQTT 3.1.1 §3.1.2.1 — must be exactly "MQTT" */
    char proto[8] = {0};
    if (read_str(b, len, &pos, proto, sizeof(proto)) < 0) {
        return -1;
    }
    if (strcmp(proto, "MQTT") != 0) {
        return -1;
    }

    if (pos + 4 > len) {
        return -1;
    }
    uint8_t proto_level = b[pos++]; /* must be 4 for MQTT 3.1.1 */
    if (proto_level != 4) {
        return -2; /* valid "MQTT" name but unsupported level — caller sends CONNACK 0x01 */
    }

    uint8_t connect_flags  = b[pos++];
    /* MQTT 3.1.1 §3.1.2.1: reserved bit (bit 0) MUST be 0 */
    if (connect_flags & 0x01) {
        return -1;
    }
    out->clean_session     = (connect_flags >> 1) & 0x01;
    uint8_t has_will       = (connect_flags >> 2) & 0x01;
    out->will_qos          = (connect_flags >> 3) & 0x03;
    out->will_retain       = (connect_flags >> 5) & 0x01;
    out->has_will          = has_will;
    out->has_password      = (connect_flags >> 6) & 0x01;
    out->has_username      = (connect_flags >> 7) & 0x01;

    /* MQTT 3.1.1 §3.1.2.4: if has_will=0, will_qos and will_retain MUST be 0 */
    if (!has_will && (out->will_qos != 0 || out->will_retain != 0)) {
        return -1;
    }
    /* MQTT 3.1.1 §3.1.2.9: password flag requires username flag */
    if (out->has_password && !out->has_username) {
        return -1;
    }

    out->keepalive = read_u16(b + pos);
    pos += 2;

    /* payload: client ID */
    if (read_str(b, len, &pos, out->client_id, sizeof(out->client_id)) < 0) {
        return -1;
    }

    if (has_will) {
        if (read_str(b, len, &pos, out->will_topic, sizeof(out->will_topic)) < 0) {
            return -1;
        }
        if (pos + 2 > len) {
            return -1;
        }
        out->will_payload_len = read_u16(b + pos);
        pos += 2;
        if (pos + out->will_payload_len > len ||
            out->will_payload_len > sizeof(out->will_payload)) {
            return -1;
        }
        memcpy(out->will_payload, b + pos, out->will_payload_len);
        pos += out->will_payload_len;
    }

    if (out->has_username) {
        if (read_str(b, len, &pos, out->username, sizeof(out->username)) < 0) {
            return -1;
        }
    }
    if (out->has_password) {
        if (read_str(b, len, &pos, out->password, sizeof(out->password)) < 0) {
            return -1;
        }
    }
    if (pos != len) {
        return -1;
    }
    return 0;
}

int packet_parse_publish(const mqtt_packet_t *pkt, mqtt_publish_t *out)
{
    const uint8_t *b   = pkt->buf;
    size_t         len = pkt->buf_len;
    size_t         pos = 0;

    out->dup    = (pkt->type_flags >> 3) & 0x01;
    out->qos    = (pkt->type_flags >> 1) & 0x03;
    out->retain = pkt->type_flags & 0x01;

    if (read_str(b, len, &pos, out->topic, sizeof(out->topic)) < 0) {
        return -1;
    }

    if (out->qos > 0) {
        if (pos + 2 > len) {
            return -1;
        }
        out->packet_id = read_u16(b + pos);
        pos += 2;
    }

    out->payload_len = (uint16_t)(len - pos);
    if (out->payload_len > sizeof(out->payload)) {
        return -1;
    }
    memcpy(out->payload, b + pos, out->payload_len);
    return 0;
}

int packet_parse_subscribe(const mqtt_packet_t *pkt,
                            uint16_t *packet_id,
                            char topics[][MQTT_TOPIC_MAX],
                            uint8_t *qos, uint8_t *count, uint8_t max_topics)
{
    const uint8_t *b   = pkt->buf;
    size_t         len = pkt->buf_len;
    size_t         pos = 0;

    if (pos + 2 > len) {
        return -1;
    }
    *packet_id = read_u16(b + pos);
    pos += 2;
    *count = 0;

    while (pos < len) {
        if (*count >= max_topics) {
            return -1;
        }
        if (read_str(b, len, &pos, topics[*count], MQTT_TOPIC_MAX) < 0) {
            return -1;
        }
        if (pos >= len) {
            return -1;
        }
        uint8_t requested_qos = b[pos++];
        if ((requested_qos & 0xFC) != 0 || requested_qos > 2) {
            return -1;
        }
        qos[*count] = requested_qos;
        (*count)++;
    }
    return 0;
}

int packet_parse_unsubscribe(const mqtt_packet_t *pkt,
                              uint16_t *packet_id,
                              char topics[][MQTT_TOPIC_MAX],
                              uint8_t *count, uint8_t max_topics)
{
    const uint8_t *b   = pkt->buf;
    size_t         len = pkt->buf_len;
    size_t         pos = 0;

    if (pos + 2 > len) {
        return -1;
    }
    *packet_id = read_u16(b + pos);
    pos += 2;
    *count = 0;

    while (pos < len) {
        if (*count >= max_topics) {
            return -1;
        }
        if (read_str(b, len, &pos, topics[*count], MQTT_TOPIC_MAX) < 0) {
            return -1;
        }
        (*count)++;
    }
    return 0;
}

/* ---------- build outgoing ---------- */

int packet_build_connack(uint8_t session_present, uint8_t return_code,
                          uint8_t *out, size_t out_cap)
{
    if (!out || out_cap < 4 || return_code > CONNACK_NOT_AUTHORIZED ||
        (return_code != CONNACK_ACCEPTED && session_present != 0)) {
        return -1;
    }
    out[0] = MQTT_CONNACK;
    out[1] = 2;
    out[2] = session_present & 0x01;
    out[3] = return_code;
    return 4;
}

int packet_build_suback(uint16_t packet_id,
                         const uint8_t *return_codes, uint8_t count,
                         uint8_t *out, size_t out_cap)
{
    uint8_t i;

    if (packet_id == 0 || count == 0 || !return_codes || !out ||
        out_cap < (size_t)(4 + count)) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (return_codes[i] != 0x00 && return_codes[i] != 0x01 &&
            return_codes[i] != 0x02 && return_codes[i] != 0x80) {
            return -1;
        }
    }
    out[0] = MQTT_SUBACK;
    out[1] = (uint8_t)(2 + count);
    write_u16(out + 2, packet_id);
    memcpy(out + 4, return_codes, count);
    return (int)(4 + count);
}

int packet_build_unsuback(uint16_t packet_id, uint8_t *out, size_t out_cap)
{
    if (packet_id == 0 || !out || out_cap < 4) {
        return -1;
    }
    out[0] = MQTT_UNSUBACK;
    out[1] = 2;
    write_u16(out + 2, packet_id);
    return 4;
}

int packet_build_publish(const mqtt_publish_t *pub, uint8_t *out, size_t out_cap)
{
    size_t   topic_len_sz;
    if (!pub || !out) {
        return -1;
    }
    if (bounded_strlen(pub->topic, sizeof(pub->topic), &topic_len_sz) < 0 ||
        topic_len_sz == 0 || strpbrk(pub->topic, "#+") ||
        pub->qos > 2 || (pub->qos > 0 && pub->packet_id == 0) ||
        pub->payload_len > sizeof(pub->payload)) {
        return -1;
    }
    uint16_t topic_len  = (uint16_t)topic_len_sz;
    size_t   var_hdr_sz = 2 + topic_len + (pub->qos > 0 ? 2 : 0);
    uint32_t rem_len    = (uint32_t)(var_hdr_sz + pub->payload_len);

    uint8_t  rem_enc[4];
    size_t   rem_bytes;
    if (packet_encode_remaining_len(rem_len, rem_enc, &rem_bytes) < 0) {
        return -1;
    }

    size_t total = 1 + rem_bytes + rem_len;
    if (out_cap < total) {
        return -1;
    }

    size_t pos = 0;
    out[pos++] = MQTT_PUBLISH | ((pub->dup & 0x01) << 3) | (pub->qos << 1) | pub->retain;
    memcpy(out + pos, rem_enc, rem_bytes);
    pos += rem_bytes;
    pos  = write_str(out, pos, pub->topic, topic_len);
    if (pub->qos > 0) {
        write_u16(out + pos, pub->packet_id);
        pos += 2;
    }
    memcpy(out + pos, pub->payload, pub->payload_len);
    pos += pub->payload_len;
    return (int)pos;
}

int packet_build_puback(uint16_t packet_id, uint8_t *out, size_t out_cap)
{
    if (packet_id == 0 || !out || out_cap < 4) {
        return -1;
    }
    out[0] = MQTT_PUBACK;
    out[1] = 2;
    write_u16(out + 2, packet_id);
    return 4;
}

int packet_build_pubrec(uint16_t packet_id, uint8_t *out, size_t out_cap)
{
    if (packet_id == 0 || !out || out_cap < 4) return -1;
    out[0] = MQTT_PUBREC;
    out[1] = 2;
    write_u16(out + 2, packet_id);
    return 4;
}

int packet_build_pubrel(uint16_t packet_id, uint8_t *out, size_t out_cap)
{
    if (packet_id == 0 || !out || out_cap < 4) return -1;
    out[0] = MQTT_PUBREL | 0x02; /* fixed header 0x62 per spec */
    out[1] = 2;
    write_u16(out + 2, packet_id);
    return 4;
}

int packet_build_pubcomp(uint16_t packet_id, uint8_t *out, size_t out_cap)
{
    if (packet_id == 0 || !out || out_cap < 4) return -1;
    out[0] = MQTT_PUBCOMP;
    out[1] = 2;
    write_u16(out + 2, packet_id);
    return 4;
}

int packet_build_pingresp(uint8_t *out, size_t out_cap)
{
    if (!out || out_cap < 2) {
        return -1;
    }
    out[0] = MQTT_PINGRESP;
    out[1] = 0;
    return 2;
}
