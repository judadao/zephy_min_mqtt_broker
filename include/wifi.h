#ifndef WIFI_H
#define WIFI_H

#ifdef __ZEPHYR__
/*
 * Zephyr standalone helper.
 *
 * Connect to WiFi and obtain an IP address before broker_init() is called.
 * Blocks until connected or returns a negative error code on failure. Product
 * embedders may use their own network bring-up instead of this helper.
 */
int wifi_connect(void);
#endif

#endif /* WIFI_H */
