#ifndef WIFI_H
#define WIFI_H

#ifdef __ZEPHYR__
/* Connect to WiFi and obtain an IP address.
 * Blocks until connected or returns negative errno on failure. */
int wifi_connect(void);
#endif

#endif /* WIFI_H */
