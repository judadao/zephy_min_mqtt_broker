#ifndef HTTP_H
#define HTTP_H

#ifndef __ZEPHYR__
int  http_server_start(uint16_t port);
void http_server_stop(void);
#endif

#endif /* HTTP_H */
