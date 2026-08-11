/* MQTT + LTE connection configuration.
 * Broker públic HiveMQ (TLS/RSA) per validar la pipeline abans del broker propi.
 */

#define MQTT_BROKER_HOSTNAME "broker.hivemq.com"
#define MQTT_BROKER_IP "3.69.77.221"
#define MQTT_BROKER_PORT 8883
#define MQTT_DEVICE_ID "nrf1"
#define MQTT_TOPIC "nrf9151/data"

#define MQTT_BROKER_USERNAME ""
#define MQTT_BROKER_PASSWORD ""

#define MQTT_TLS_SEC_TAG 955

#define LTE_APN "internet.m2mportal.de"
