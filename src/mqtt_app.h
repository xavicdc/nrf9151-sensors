#ifndef MQTT_APP_H
#define MQTT_APP_H

#include <stdbool.h>

int mqtt_app_init(void);
int mqtt_app_start(void);
int mqtt_app_gnss_acquire(int timeout_sec);
int mqtt_app_publish(const char *json);
bool mqtt_app_connected(void);

#endif /* MQTT_APP_H */
