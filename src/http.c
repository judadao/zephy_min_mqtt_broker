#ifndef __ZEPHYR__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include "http.h"
#include "broker.h"
#include "client.h"
#include "topic.h"
#include "packet.h"
#include "platform/platform.h"

/* ── embedded dashboard HTML ───────────────────────────────────────────────── */

static const char DASHBOARD[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\"><head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>MQTT Broker</title>\n"
"<style>\n"
"body{font-family:sans-serif;margin:24px;background:#f0f2f5;color:#222}\n"
"h1{margin-bottom:4px}h2{color:#444;border-bottom:1px solid #ccc;padding-bottom:4px}\n"
"table{border-collapse:collapse;width:100%;background:#fff;margin-bottom:20px;border-radius:6px;overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,.1)}\n"
"th,td{border:1px solid #e0e0e0;padding:8px 14px;text-align:left}\n"
"th{background:#4a90e2;color:#fff;font-weight:600}\n"
"tr:nth-child(even){background:#f7f9fc}\n"
".bar{display:flex;gap:16px;margin-bottom:16px;flex-wrap:wrap}\n"
".card{background:#fff;border-radius:6px;padding:12px 18px;box-shadow:0 1px 3px rgba(0,0,0,.1);min-width:120px}\n"
".card span{display:block;font-size:22px;font-weight:700;color:#4a90e2}\n"
".card small{color:#888;font-size:12px}\n"
".dot{width:10px;height:10px;border-radius:50%;display:inline-block;background:#ccc;margin-right:6px;vertical-align:middle}\n"
".ok{background:#4caf50}.err{background:#f44336}\n"
".row{display:flex;gap:8px;align-items:center;margin-bottom:8px}\n"
"input,select{padding:7px 10px;border:1px solid #ccc;border-radius:4px;font-size:14px}\n"
"button{padding:7px 18px;background:#4a90e2;color:#fff;border:none;border-radius:4px;cursor:pointer;font-size:14px}\n"
"button:hover{background:#357abd}\n"
"#msg{font-size:13px;color:#4caf50;margin-left:8px}\n"
"</style></head>\n"
"<body>\n"
"<h1>MQTT Broker Dashboard</h1>\n"
"<div style=\"margin-bottom:12px\"><span class=\"dot\" id=\"dot\"></span><span id=\"ts\" style=\"color:#888;font-size:13px\"></span></div>\n"
"<div class=\"bar\">\n"
"  <div class=\"card\"><span id=\"v-up\">—</span><small>Uptime (s)</small></div>\n"
"  <div class=\"card\"><span id=\"v-cl\">—</span><small>Clients</small></div>\n"
"  <div class=\"card\"><span id=\"v-sb\">—</span><small>Subscriptions</small></div>\n"
"  <div class=\"card\"><span id=\"v-rt\">—</span><small>Retained</small></div>\n"
"</div>\n"
"<h2>Connected Clients</h2>\n"
"<table><thead><tr><th>Slot</th><th>Client ID</th><th>Keepalive (s)</th><th>Idle (ms)</th></tr></thead>\n"
"<tbody id=\"tb-c\"></tbody></table>\n"
"<h2>Subscriptions</h2>\n"
"<table><thead><tr><th>Filter</th><th>Client ID</th><th>QoS</th></tr></thead>\n"
"<tbody id=\"tb-s\"></tbody></table>\n"
"<h2>Retained Messages</h2>\n"
"<table><thead><tr><th>Topic</th><th>Payload (bytes)</th><th>QoS</th></tr></thead>\n"
"<tbody id=\"tb-r\"></tbody></table>\n"
"<h2>Publish</h2>\n"
"<div class=\"row\">\n"
"  <input id=\"p-topic\" placeholder=\"topic\" style=\"width:180px\">\n"
"  <input id=\"p-payload\" placeholder=\"payload\" style=\"width:240px\">\n"
"  <select id=\"p-qos\"><option value=\"0\">QoS 0</option><option value=\"1\">QoS 1</option></select>\n"
"  <button onclick=\"pub()\">Publish</button>\n"
"  <span id=\"msg\"></span>\n"
"</div>\n"
"<script>\n"
"var dot=document.getElementById('dot');\n"
"function tbl(id,rows,keys){\n"
"  var b=document.getElementById(id);b.innerHTML='';\n"
"  if(!rows.length){var r=document.createElement('tr'),d=document.createElement('td');\n"
"    d.colSpan=keys.length;d.style.color='#aaa';d.style.textAlign='center';\n"
"    d.textContent='(none)';r.appendChild(d);b.appendChild(r);return;}\n"
"  rows.forEach(function(row){\n"
"    var r=document.createElement('tr');\n"
"    keys.forEach(function(k){var d=document.createElement('td');d.textContent=row[k]!=null?row[k]:'';r.appendChild(d);});\n"
"    b.appendChild(r);});\n"
"}\n"
"function refresh(){\n"
"  fetch('/api/status').then(function(r){return r.json();}).then(function(d){\n"
"    dot.className='dot ok';\n"
"    document.getElementById('ts').textContent=new Date().toLocaleTimeString();\n"
"    document.getElementById('v-up').textContent=Math.floor(d.uptime_ms/1000);\n"
"    document.getElementById('v-cl').textContent=d.clients.length;\n"
"    document.getElementById('v-sb').textContent=d.subscriptions.length;\n"
"    document.getElementById('v-rt').textContent=d.retained.length;\n"
"    tbl('tb-c',d.clients,['slot','client_id','keepalive','idle_ms']);\n"
"    tbl('tb-s',d.subscriptions,['filter','client_id','qos']);\n"
"    tbl('tb-r',d.retained,['topic','payload_len','qos']);\n"
"  }).catch(function(){dot.className='dot err';});\n"
"}\n"
"function pub(){\n"
"  var t=document.getElementById('p-topic').value;\n"
"  var p=document.getElementById('p-payload').value;\n"
"  var q=parseInt(document.getElementById('p-qos').value);\n"
"  if(!t){document.getElementById('msg').textContent='Topic required';return;}\n"
"  fetch('/api/publish',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"    body:JSON.stringify({topic:t,payload:p,qos:q})})\n"
"  .then(function(r){return r.json();})\n"
"  .then(function(d){\n"
"    var m=document.getElementById('msg');\n"
"    m.textContent=d.ok?'Published ✓':'Error: '+d.error;\n"
"    m.style.color=d.ok?'#4caf50':'#f44336';\n"
"    setTimeout(function(){m.textContent='';},2000);\n"
"  }).catch(function(){document.getElementById('msg').textContent='Request failed';});\n"
"}\n"
"setInterval(refresh,2000);refresh();\n"
"</script></body></html>\n";

/* ── HTTP helpers ───────────────────────────────────────────────────────────── */

static void http_send(int fd, int code, const char *ctype,
                      const char *body, size_t blen)
{
    char hdr[256];
    const char *reason = (code == 200) ? "OK" :
                         (code == 400) ? "Bad Request" : "Not Found";
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n", code, reason, ctype, blen);
    send(fd, hdr, (size_t)hlen, 0);
    if (blen) send(fd, body, blen, 0);
}

/* extract first value of "key":"..." from json (in-place, no alloc) */
static int json_str(const char *json, const char *key, char *out, size_t cap)
{
    char search[72];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
    return 0;
}

static int json_int(const char *json, const char *key)
{
    char search[72];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    return atoi(p);
}

/* ── request handlers ───────────────────────────────────────────────────────── */

static void handle_status(int fd)
{
    client_snapshot_t  cs[MQTT_MAX_CLIENTS];
    sub_snapshot_t     ss[TOPIC_MAX_SUBS];
    retain_snapshot_t  rs[TOPIC_MAX_SUBS];

    int nc = client_get_snapshots(cs, MQTT_MAX_CLIENTS);
    int ns = topic_get_sub_snapshots(ss, TOPIC_MAX_SUBS);
    int nr = topic_get_retain_snapshots(rs, TOPIC_MAX_SUBS);

    int64_t now = plat_uptime_ms();
    char buf[8192];
    int  pos = 0;

#define APPEND(fmt, ...) pos += snprintf(buf + pos, (int)sizeof(buf) - pos, fmt, ##__VA_ARGS__)

    APPEND("{\"uptime_ms\":%lld", (long long)now);

    APPEND(",\"clients\":[");
    for (int i = 0; i < nc; i++) {
        if (i) APPEND(",");
        APPEND("{\"slot\":%d,\"client_id\":\"%s\",\"keepalive\":%d,\"idle_ms\":%lld}",
               cs[i].slot, cs[i].client_id, cs[i].keepalive,
               (long long)(now - cs[i].last_seen_ms));
    }
    APPEND("]");

    APPEND(",\"subscriptions\":[");
    for (int i = 0; i < ns; i++) {
        if (i) APPEND(",");
        APPEND("{\"filter\":\"%s\",\"client_id\":\"%s\",\"qos\":%d}",
               ss[i].filter, ss[i].client_id, ss[i].qos);
    }
    APPEND("]");

    APPEND(",\"retained\":[");
    for (int i = 0; i < nr; i++) {
        if (i) APPEND(",");
        APPEND("{\"topic\":\"%s\",\"payload_len\":%d,\"qos\":%d}",
               rs[i].topic, rs[i].payload_len, rs[i].qos);
    }
    APPEND("]}");

#undef APPEND

    http_send(fd, 200, "application/json", buf, (size_t)pos);
}

static void handle_publish_api(int fd, const char *body)
{
    char topic[MQTT_TOPIC_MAX]   = {0};
    char payload[MQTT_PAYLOAD_MAX] = {0};

    if (json_str(body, "topic", topic, sizeof(topic)) < 0 || topic[0] == '\0') {
        http_send(fd, 400, "application/json", "{\"ok\":false,\"error\":\"missing topic\"}", 36);
        return;
    }
    json_str(body, "payload", payload, sizeof(payload));
    int qos = json_int(body, "qos");
    if (qos < 0 || qos > 1) qos = 0;

    mqtt_publish_t pub = {0};
    strncpy(pub.topic, topic, sizeof(pub.topic) - 1);
    pub.payload_len = (uint16_t)strlen(payload);
    memcpy(pub.payload, payload, pub.payload_len);
    pub.qos    = (uint8_t)qos;
    pub.retain = 0;

    topic_publish(&pub);

    http_send(fd, 200, "application/json", "{\"ok\":true}", 11);
}

/* ── connection dispatch ────────────────────────────────────────────────────── */

static void handle_connection(int fd)
{
    char req[4096] = {0};
    int  total     = 0;

    /* read until end of headers (or buffer full) */
    while (total < (int)sizeof(req) - 1) {
        ssize_t n = recv(fd, req + total, sizeof(req) - 1 - (size_t)total, 0);
        if (n <= 0) return;
        total += (int)n;
        if (strstr(req, "\r\n\r\n")) break;
    }

    /* OPTIONS preflight (CORS) */
    if (strncmp(req, "OPTIONS", 7) == 0) {
        http_send(fd, 200, "text/plain", "", 0);
        return;
    }

    /* route on path */
    if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /\r", 6) == 0) {
        http_send(fd, 200, "text/html; charset=utf-8",
                  DASHBOARD, sizeof(DASHBOARD) - 1);
        return;
    }

    if (strncmp(req, "GET /api/status", 15) == 0) {
        handle_status(fd);
        return;
    }

    if (strncmp(req, "POST /api/publish", 17) == 0) {
        /* find body after \r\n\r\n */
        const char *body = strstr(req, "\r\n\r\n");
        if (body) body += 4;

        /* if body was cut off, read remaining Content-Length bytes */
        int clen = 0;
        const char *cl = strstr(req, "Content-Length:");
        if (!cl) cl = strstr(req, "content-length:");
        if (cl) clen = atoi(cl + 15);

        static char body_buf[MQTT_PAYLOAD_MAX + 256];
        if (body) {
            int already = total - (int)(body - req);
            int remain  = clen - already;
            if (already > 0)
                memcpy(body_buf, body, (size_t)already < sizeof(body_buf) ? (size_t)already : sizeof(body_buf) - 1);
            if (remain > 0 && already + remain < (int)sizeof(body_buf) - 1) {
                recv(fd, body_buf + already, (size_t)remain, MSG_WAITALL);
            }
            body_buf[already + (remain > 0 ? remain : 0)] = '\0';
        } else {
            body_buf[0] = '\0';
        }
        handle_publish_api(fd, body_buf);
        return;
    }

    http_send(fd, 404, "application/json", "{\"error\":\"not found\"}", 21);
}

/* ── server thread ──────────────────────────────────────────────────────────── */

static int      srv_fd   = -1;
static volatile int running = 0;
static pthread_t http_tid;

static void *http_thread(void *arg)
{
    (void)arg;
    while (running) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(srv_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            if (running) perror("http accept");
            break;
        }
        handle_connection(fd);
        close(fd);
    }
    return NULL;
}

int http_server_start(uint16_t port)
{
    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return -1;

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
    };
    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv_fd); return -1;
    }
    listen(srv_fd, 4);

    running = 1;
    pthread_create(&http_tid, NULL, http_thread, NULL);
    printf("[INF] HTTP dashboard on http://0.0.0.0:%u\n", port);
    return 0;
}

void http_server_stop(void)
{
    running = 0;
    if (srv_fd >= 0) { close(srv_fd); srv_fd = -1; }
    pthread_join(http_tid, NULL);
}

#endif /* !__ZEPHYR__ */
