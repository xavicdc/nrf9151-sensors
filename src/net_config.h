/* MQTT + LTE connection configuration.
 * Broker públic HiveMQ (TLS/RSA) per validar la pipeline abans del broker propi.
 */

#define MQTT_BROKER_HOSTNAME "broker.hivemq.com"
#define MQTT_BROKER_IP "3.69.77.221"
#define MQTT_BROKER_PORT 8883
#define MQTT_DEVICE_ID "nrf1"
#define MQTT_TOPIC "nrf9151/data"

/* Cadència de publicació. Pla terrestre Onomondo 50 MB/mes: cada publicació
 * costa ~160 B + sobrecost LTE-M/NB-IoT. Amb 300 s (5 min) ≈ 46 KB/mes. */
#define MQTT_PUBLISH_INTERVAL_SECONDS 300

/* NTN NB-IoT (satèl·lit GEO/LEO).
 * IMPORTANT: requereix el firmware de mòdem mfw_nrf9151-ntn flashejat (un
 * firmware diferent del terrestre; no es poden usar tots dos alhora) i que el
 * pla satel·lital estigui actiu. El pla és 50 KB/mes: ~160 B per missatge =>
 * ~1 missatge cada 3 h és prudent. */
#define MQTT_USE_NTN_NBIOT 0
#define MQTT_PUBLISH_INTERVAL_SECONDS_NTN 10800

/* Mode GNSS-únic (prova): el mòdem dedica tot l'RF al GNSS i no fa LTE.
 * Deixar a 0: l'alternança automàtica (mqtt_app_gnss_acquire) ja fa bursts
 * GNSS-únic cada GNSS_ACQUIRE_INTERVAL_SECONDS per obtenir fix net. */
#define MQTT_USE_GNSS_ONLY 0

/* Alternança de modes: cada interval, es fa un burst GNSS-únic per obtenir
 * un fix net (sense contenció LTE), i després es torna a LTE-M+NB-IoT+GPS
 * per transmetre. La posició queda emmagatzemada entre adquisicions. */
#define GNSS_ACQUIRE_INTERVAL_SECONDS 300
#define GNSS_ACQUIRE_TIMEOUT_SECONDS 120

#define MQTT_BROKER_USERNAME ""
#define MQTT_BROKER_PASSWORD ""

#define MQTT_TLS_SEC_TAG 955

#define LTE_APN "onomondo"
