/* MQTT + LTE connection configuration.
 * Broker públic HiveMQ (TLS/RSA) per validar la pipeline abans del broker propi.
 */

#define MQTT_BROKER_HOSTNAME "broker.hivemq.com"
#define MQTT_BROKER_IP "3.69.77.221"
#define MQTT_BROKER_PORT 8883
#define MQTT_DEVICE_ID "nrf1"
#define MQTT_TOPIC "nrf9151/data"

/* Cadència de publicació. El pla terrestre és 6.5 MB/mes: cada publicació
 * costa ~160 B + sobrecost LTE-M, així que 60 s ≈ 230 KB/mes (molt segur). */
#define MQTT_PUBLISH_INTERVAL_SECONDS 60

/* NTN NB-IoT (satèl·lit GEO/LEO).
 * IMPORTANT: requereix el firmware de mòdem mfw_nrf9151-ntn flashejat (un
 * firmware diferent del terrestre; no es poden usar tots dos alhora) i que el
 * pla satel·lital estigui actiu. El pla és 50 KB/mes: ~160 B per missatge =>
 * ~1 missatge cada 3 h és prudent. */
#define MQTT_USE_NTN_NBIOT 0
#define MQTT_PUBLISH_INTERVAL_SECONDS_NTN 10800

#define MQTT_BROKER_USERNAME ""
#define MQTT_BROKER_PASSWORD ""

#define MQTT_TLS_SEC_TAG 955

#define LTE_APN "internet.m2mportal.de"
