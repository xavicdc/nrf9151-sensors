#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include <modem/modem_key_mgmt.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>

#include "net_config.h"
#include "mqtt_app.h"

LOG_MODULE_REGISTER(credentials, LOG_LEVEL_INF);

#define TLS_SEC_TAG MQTT_TLS_SEC_TAG

#include "ca_cert.h"

static void credentials_provision(void)
{
	int err;

	err = modem_key_mgmt_write(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   ca_certificate, sizeof(ca_certificate) - 1);
	if (err) {
		printk("Failed to provision CA cert: %d\n", err);
	} else {
		printk("CA cert provisioned\n");
	}

	err = nrf_modem_at_cmd(NULL, 0, "AT+CGDCONT=1,\"IP\",\"%s\"", LTE_APN);
	if (err) {
		printk("Failed to set APN: %d\n", err);
	} else {
		printk("APN set: %s\n", LTE_APN);
	}
}

static void on_modem_lib_init(int ret, void *ctx)
{
	ARG_UNUSED(ctx);

	printk("Modem library init result: %d\n", ret);

	if (ret != 0) {
		return;
	}

	credentials_provision();
	mqtt_app_start();
}

NRF_MODEM_LIB_ON_INIT(credentials_hook, on_modem_lib_init, NULL);
