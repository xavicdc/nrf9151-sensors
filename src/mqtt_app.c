#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>

#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/modem_key_mgmt.h>

#include <string.h>

#include "mqtt_app.h"
#include "net_config.h"
#include "gnss.h"
#include "agnss.h"

LOG_MODULE_REGISTER(mqtt_app, LOG_LEVEL_INF);

#define MQTT_APP_STACK_SIZE 4096
#define MQTT_APP_THREAD_PRIORITY 3
#define MQTT_APP_QUEUE_SIZE 4
#define MQTT_APP_PAYLOAD_SIZE 160

struct payload_item {
	char data[MQTT_APP_PAYLOAD_SIZE];
	size_t len;
};

K_SEM_DEFINE(net_ready_sem, 0, 1);
K_SEM_DEFINE(connected_sem, 0, 1);
K_MSGQ_DEFINE(payload_q, sizeof(struct payload_item), MQTT_APP_QUEUE_SIZE, 4);

static volatile bool mqtt_connected;
static volatile bool network_up;
static bool lte_started;

static struct mqtt_client mqtt_client;
static struct sockaddr_storage broker;
static struct mqtt_utf8 user_name;
static struct mqtt_utf8 password;
static uint8_t rx_buffer[512];
static uint8_t tx_buffer[512];
static sec_tag_t sec_tag_list[] = { MQTT_TLS_SEC_TAG };

static void mqtt_evt_handler(struct mqtt_client *client, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->param.connack.return_code == MQTT_CONNECTION_ACCEPTED) {
			printk("MQTT connected\n");
			mqtt_connected = true;
			k_sem_give(&connected_sem);
		} else {
			printk("MQTT CONNACK refused, code: %d\n",
			       evt->param.connack.return_code);
			mqtt_connected = false;
		}
		break;
	case MQTT_EVT_DISCONNECT:
		printk("MQTT disconnected\n");
		mqtt_connected = false;
		break;
	default:
		break;
	}
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		switch (evt->nw_reg_status) {
		case LTE_LC_NW_REG_REGISTERED_HOME:
		case LTE_LC_NW_REG_REGISTERED_ROAMING:
			printk("LTE network registered\n");
			network_up = true;
			k_sem_give(&net_ready_sem);
			break;
		case LTE_LC_NW_REG_SEARCHING:
			printk("LTE: searching for network...\n");
			network_up = false;
			break;
		case LTE_LC_NW_REG_NOT_REGISTERED:
			printk("LTE: not registered\n");
			network_up = false;
			break;
		case LTE_LC_NW_REG_REGISTRATION_DENIED:
			printk("LTE: registration denied\n");
			network_up = false;
			break;
		case LTE_LC_NW_REG_UICC_FAIL:
			printk("LTE: SIM/UICC failure (check SIM card)\n");
			network_up = false;
			break;
		case LTE_LC_NW_REG_NO_SUITABLE_CELL:
			printk("LTE: no suitable cell (check antenna/coverage)\n");
			network_up = false;
			break;
		default:
			printk("LTE: registration status %d\n", evt->nw_reg_status);
			break;
		}
		break;
	default:
		break;
	}
}

int mqtt_app_start(void)
{
	int err;

	if (lte_started) {
		return 0;
	}

	printk("--- Modem setup ---\n");

	for (int i = 0; i < 10; i++) {
		if (nrf_modem_at_cmd(NULL, 0, "AT") == 0) {
			printk("Modem AT ready\n");
			break;
		}
		k_sleep(K_SECONDS(1));
	}

	err = lte_lc_system_mode_set(IS_ENABLED(MQTT_USE_NTN_NBIOT)
				     ? LTE_LC_SYSTEM_MODE_NTN_NBIOT
				     : IS_ENABLED(MQTT_USE_GNSS_ONLY)
				     ? LTE_LC_SYSTEM_MODE_GPS
				     : LTE_LC_SYSTEM_MODE_LTEM_GPS,
				     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	printk("system_mode_set = %d\n", err);

	enum lte_lc_system_mode cur_mode;
	enum lte_lc_system_mode_preference cur_pref;

	if (lte_lc_system_mode_get(&cur_mode, &cur_pref) == 0) {
		printk("system mode now: %d\n", cur_mode);
	}

	err = nrf_modem_at_cmd(NULL, 0, "AT+CGDCONT=1,\"IP\",\"%s\"", LTE_APN);
	if (err) {
		printk("APN set failed: %d\n", err);
	} else {
		printk("APN set: %s\n", LTE_APN);
	}

	lte_lc_register_handler(lte_handler);
	err = lte_lc_connect_async(NULL);
	if (err) {
		printk("lte_lc_connect_async failed: %d\n", err);
		return err;
	}

	lte_started = true;
	printk("LTE connect started\n");
	return 0;
}

static int broker_init(struct sockaddr_storage *storage)
{
	struct sockaddr_in *broker4 = (struct sockaddr_in *)storage;

	if (zsock_inet_pton(AF_INET, MQTT_BROKER_IP, &broker4->sin_addr) == 1) {
		broker4->sin_family = AF_INET;
		broker4->sin_port = htons(MQTT_BROKER_PORT);
		printk("Using static broker IP %s:%d\n", MQTT_BROKER_IP, MQTT_BROKER_PORT);
		return 0;
	}

	struct zsock_addrinfo hints = {
		.ai_socktype = SOCK_STREAM,
	};
	struct zsock_addrinfo *addr;
	struct zsock_addrinfo *ipv6 = NULL;
	struct zsock_addrinfo *result = NULL;
	int err;

	err = zsock_getaddrinfo(MQTT_BROKER_HOSTNAME, NULL, &hints, &result);
	if (err) {
		printk("getaddrinfo failed: %d\n", err);
		return -err;
	}

	for (addr = result; addr != NULL; addr = addr->ai_next) {
		if (addr->ai_family == AF_INET) {
			memcpy(broker4, addr->ai_addr, sizeof(*broker4));
			broker4->sin_port = htons(MQTT_BROKER_PORT);
			broker4->sin_family = AF_INET;
			zsock_freeaddrinfo(result);
			printk("Broker resolved to IPv4\n");
			return 0;
		} else if (addr->ai_family == AF_INET6) {
			struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)addr->ai_addr;

			if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
				memcpy(&broker4->sin_addr, &a6->sin6_addr.s6_addr[12], 4);
				broker4->sin_family = AF_INET;
				broker4->sin_port = htons(MQTT_BROKER_PORT);
				zsock_freeaddrinfo(result);
				printk("Broker resolved to IPv4 (mapped)\n");
				return 0;
			}

			if (ipv6 == NULL) {
				ipv6 = addr;
			}
		}
	}

	if (ipv6 != NULL) {
		struct sockaddr_in6 *broker6 = (struct sockaddr_in6 *)storage;

		memcpy(broker6, ipv6->ai_addr, sizeof(*broker6));
		broker6->sin6_port = htons(MQTT_BROKER_PORT);
		broker6->sin6_family = AF_INET6;
		zsock_freeaddrinfo(result);
		printk("Broker resolved to IPv6\n");
		return 0;
	}

	zsock_freeaddrinfo(result);
	printk("No usable address found\n");
	return -ENOENT;
}

static int mqtt_connect_once(void)
{
	int err;

	bool ca_exists = false;

	if (modem_key_mgmt_exists(MQTT_TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				  &ca_exists) == 0 && ca_exists) {
		printk("CA cert present in modem\n");
	} else {
		printk("WARNING: CA cert NOT present in modem\n");
	}

	enum lte_lc_nw_reg_status rs;

	if (lte_lc_nw_reg_status_get(&rs) == 0) {
		printk("LTE reg status: %d\n", rs);
	}

	char pdn_buf[96];
	int pdn_err = nrf_modem_at_cmd(pdn_buf, sizeof(pdn_buf), "AT+CGPADDR=1");

	if (pdn_err == 0) {
		pdn_buf[sizeof(pdn_buf) - 1] = '\0';
		printk("PDN addr: %s\n", pdn_buf);
	} else {
		printk("PDN addr query failed: %d\n", pdn_err);
	}

	err = broker_init(&broker);
	if (err) {
		return err;
	}

	mqtt_client_init(&mqtt_client);

	mqtt_client.broker = (struct sockaddr *)&broker;
	mqtt_client.evt_cb = mqtt_evt_handler;
	user_name.utf8 = MQTT_BROKER_USERNAME;
	user_name.size = strlen(MQTT_BROKER_USERNAME);
	password.utf8 = MQTT_BROKER_PASSWORD;
	password.size = strlen(MQTT_BROKER_PASSWORD);

	mqtt_client.client_id = (struct mqtt_utf8){ MQTT_DEVICE_ID, strlen(MQTT_DEVICE_ID) };
	mqtt_client.user_name = &user_name;
	mqtt_client.password = &password;
	mqtt_client.protocol_version = MQTT_VERSION_3_1_1;
	mqtt_client.rx_buf = rx_buffer;
	mqtt_client.rx_buf_size = sizeof(rx_buffer);
	mqtt_client.tx_buf = tx_buffer;
	mqtt_client.tx_buf_size = sizeof(tx_buffer);

	mqtt_client.transport.type = MQTT_TRANSPORT_SECURE;

	struct mqtt_sec_config *tls = &mqtt_client.transport.tls.config;

	tls->peer_verify = TLS_PEER_VERIFY_REQUIRED;
	tls->sec_tag_list = sec_tag_list;
	tls->sec_tag_count = ARRAY_SIZE(sec_tag_list);
	tls->hostname = MQTT_BROKER_HOSTNAME;
	tls->session_cache = TLS_SESSION_CACHE_DISABLED;

	return mqtt_connect(&mqtt_client);
}

static void mqtt_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int64_t last_live_ms = 0;

	while (true) {
		int err;
		int connect_fail_count = 0;

		while (!network_up) {
			mqtt_app_start();
			k_sem_take(&net_ready_sem, K_SECONDS(5));
		}

		/* Network is up: inject A-GNSS time/location (need network time + PLMN). */
		agnss_request_force();

		k_sleep(K_SECONDS(2));

		int64_t pdn_wait = k_uptime_get();
		bool pdn_ready = false;

		while ((k_uptime_get() - pdn_wait) < 60000) {
			char pdn_buf[64];

			if (nrf_modem_at_cmd(pdn_buf, sizeof(pdn_buf), "AT+CGPADDR=1") == 0 &&
			    strchr(pdn_buf, '.') != NULL) {
				pdn_ready = true;
				break;
			}
			k_sleep(K_SECONDS(2));
		}

		printk("PDN ready: %d\n", pdn_ready ? 1 : 0);

		printk("Connecting to MQTT broker %s (TLS)\n", MQTT_BROKER_HOSTNAME);
		k_sem_reset(&connected_sem);
		mqtt_connected = false;

		err = mqtt_connect_once();
		if (err) {
			printk("mqtt_connect failed: %d\n", err);

			if (++connect_fail_count >= 5) {
				printk("Forcing LTE re-registration\n");
				lte_lc_offline();
				k_sleep(K_SECONDS(2));
				lte_lc_connect_async(NULL);
			}

			k_sleep(K_SECONDS(5));
			continue;
		}

		int64_t wait_start = k_uptime_get();

		while (!mqtt_connected && (k_uptime_get() - wait_start) < 30000) {
			struct zsock_pollfd fds = {
				.fd = mqtt_client.transport.tls.sock,
				.events = ZSOCK_POLLIN,
			};
			int ret = zsock_poll(&fds, 1, 1000);

			if (ret < 0) {
				printk("MQTT poll error: %d\n", -errno);
				break;
			}

			if (ret > 0 && (fds.revents & ZSOCK_POLLIN)) {
				if (mqtt_input(&mqtt_client) != 0) {
					printk("mqtt_input error\n");
					break;
				}
			}
		}

		if (!mqtt_connected) {
			printk("MQTT connection not established, retrying...\n");
			k_sleep(K_SECONDS(5));
			continue;
		}

		last_live_ms = k_uptime_get();

		while (mqtt_connected) {
			struct zsock_pollfd fds = {
				.fd = mqtt_client.transport.tls.sock,
				.events = ZSOCK_POLLIN,
			};
			int ret = zsock_poll(&fds, 1, 1000);

			if (ret < 0) {
				printk("MQTT poll error: %d\n", -errno);
				break;
			}

			if (ret > 0) {
				if (fds.revents & (ZSOCK_POLLERR | ZSOCK_POLLNVAL |
						   ZSOCK_POLLHUP)) {
					printk("MQTT socket error\n");
					break;
				}

				if (fds.revents & ZSOCK_POLLIN) {
					if (mqtt_input(&mqtt_client) != 0) {
						printk("mqtt_input error\n");
						break;
					}
				}
			}

			if (k_uptime_get() - last_live_ms >=
			    (CONFIG_MQTT_KEEPALIVE * 1000) / 2) {
				if (mqtt_live(&mqtt_client) != 0) {
					printk("mqtt_live error\n");
					break;
				}
				last_live_ms = k_uptime_get();
			}

			struct payload_item item;

			while (k_msgq_get(&payload_q, &item, K_NO_WAIT) == 0) {
				struct mqtt_publish_param param = {
					.message.payload.data = item.data,
					.message.payload.len = item.len,
					.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE,
					.message.topic.topic.utf8 = MQTT_TOPIC,
					.message.topic.topic.size = strlen(MQTT_TOPIC),
				};

				err = mqtt_publish(&mqtt_client, &param);
				if (err) {
					printk("Publish failed: %d\n", err);
				} else {
					printk("Published: %s\n", item.data);
					last_live_ms = k_uptime_get();
				}
			}
		}

		printk("MQTT connection lost, reconnecting...\n");
		k_sleep(K_SECONDS(5));
	}
}

K_THREAD_DEFINE(mqtt_thread_id, MQTT_APP_STACK_SIZE, mqtt_thread, NULL, NULL, NULL,
		MQTT_APP_THREAD_PRIORITY, 0, 0);

int mqtt_app_gnss_acquire(int timeout_sec)
{
	bool got_fix = false;
	int err;

	printk("GNSS acquire: going offline\n");
	network_up = false;
	err = lte_lc_offline();
	printk("offline = %d\n", err);
	k_sleep(K_SECONDS(5));

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	printk("mode(GPS) = %d\n", err);
	k_sleep(K_SECONDS(2));

	err = lte_lc_normal();
	printk("normal = %d\n", err);
	k_sleep(K_SECONDS(3));

	uint32_t before_count = gnss_fix_count_get();

	for (int i = 0; i < timeout_sec; i++) {
		if (gnss_fix_count_get() > before_count) {
			got_fix = true;
			break;
		}
		k_sleep(K_SECONDS(1));
	}

	printk("GNSS acquire: %s\n", got_fix ? "FIX" : "no fix");

	err = lte_lc_offline();
	printk("offline = %d\n", err);
	k_sleep(K_SECONDS(3));

	err = lte_lc_system_mode_set(IS_ENABLED(MQTT_USE_NTN_NBIOT)
				     ? LTE_LC_SYSTEM_MODE_NTN_NBIOT
				     : IS_ENABLED(MQTT_USE_GNSS_ONLY)
				     ? LTE_LC_SYSTEM_MODE_GPS
				     : LTE_LC_SYSTEM_MODE_LTEM_GPS,
				     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	printk("mode(LTE) = %d\n", err);
	k_sleep(K_SECONDS(2));

	err = lte_lc_normal();
	printk("normal = %d\n", err);

	err = lte_lc_connect_async(NULL);
	if (err) {
		printk("lte_lc_connect_async failed: %d\n", err);
	}

	return got_fix ? 0 : -1;
}

int mqtt_app_init(void)
{
	int err;

	err = nrf_modem_lib_init();
	if (err) {
		printk("Modem library init failed: %d\n", err);
		return err;
	}

	return 0;
}

int mqtt_app_publish(const char *json)
{
	struct payload_item item;
	size_t len = strlen(json);

	if (len >= sizeof(item.data)) {
		return -EMSGSIZE;
	}

	memcpy(item.data, json, len + 1);
	item.len = len;

	if (k_msgq_put(&payload_q, &item, K_NO_WAIT) != 0) {
		printk("Payload queue full, dropping message\n");
		return -ENOBUFS;
	}

	return 0;
}

bool mqtt_app_connected(void)
{
	return mqtt_connected;
}
