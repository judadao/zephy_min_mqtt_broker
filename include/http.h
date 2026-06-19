#ifndef HTTP_H
#define HTTP_H

#ifndef __ZEPHYR__
/*
 * Linux-only dashboard lifecycle.
 *
 * http_server_start() starts the optional status/dashboard server on port and
 * returns 0 on success or a negative error code on failure. The dashboard reads
 * broker snapshots through public snapshot APIs; it does not own broker state.
 * http_server_stop() stops the dashboard listener when enabled.
 */
int  http_server_start(uint16_t port);
void http_server_stop(void);
#endif

#endif /* HTTP_H */
