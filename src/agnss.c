/*
 * A-GNSS via nRF Cloud (REST).
 * Descarrega efemèride fresca per la xarxa cel·lular (qualsevol SIM amb
 * internet) i l'injecta al mòdem per a un fix GNSS ràpid.
 *
 * La descàrrega s'executa en un thread dedicat (la crida REST bloqueja i no es
 * pot fer des del context de l'event GNSS).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <modem/modem_info.h>
#include <modem/modem_jwt.h>
#include <net/nrf_cloud_rest.h>
#include <net/nrf_cloud_agnss.h>

#include "agnss.h"

#define AGNSS_THREAD_STACK 8192

K_SEM_DEFINE(agnss_sem, 0, 1);
static struct nrf_modem_gnss_agnss_data_frame pending_req;

static char rx_buf[2048];
static char agnss_data_buf[NRF_CLOUD_AGNSS_MAX_DATA_SIZE];
static char jwt_buf[600];

static int serving_cell_info_get(struct lte_lc_cell *serving_cell)
{
	int err;

	err = modem_info_init();
	if (err) {
		return err;
	}

	char resp_buf[MODEM_INFO_MAX_RESPONSE_SIZE];

	err = modem_info_string_get(MODEM_INFO_CELLID, resp_buf, sizeof(resp_buf));
	if (err < 0) {
		return err;
	}
	serving_cell->id = strtol(resp_buf, NULL, 16);

	err = modem_info_string_get(MODEM_INFO_AREA_CODE, resp_buf, sizeof(resp_buf));
	if (err < 0) {
		return err;
	}
	serving_cell->tac = strtol(resp_buf, NULL, 16);

	/* MODEM_INFO_OPERATOR retorna MNC+MCC junts. */
	err = modem_info_string_get(MODEM_INFO_OPERATOR, resp_buf, sizeof(resp_buf));
	if (err < 0) {
		return err;
	}
	serving_cell->mnc = strtol(&resp_buf[3], NULL, 10);
	resp_buf[3] = '\0';
	serving_cell->mcc = strtol(resp_buf, NULL, 10);

	return 0;
}

static void agnss_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		int err;

		k_sem_take(&agnss_sem, K_FOREVER);

		k_sleep(K_SECONDS(2));

		printk("AGNSS: requesting ephemerides from nRF Cloud\n");

		struct lte_lc_cells_info net_info = { 0 };

		err = serving_cell_info_get(&net_info.current_cell);
		if (err) {
			printk("AGNSS: no cell info, using generic request\n");
		}

		struct nrf_cloud_rest_agnss_request request = {
			.type = NRF_CLOUD_REST_AGNSS_REQ_CUSTOM,
			.agnss_req = &pending_req,
			.net_info = err ? NULL : &net_info,
		};

		struct nrf_cloud_rest_agnss_result result = {
			.buf = agnss_data_buf,
			.buf_sz = sizeof(agnss_data_buf),
			.agnss_sz = 0,
		};

		struct nrf_cloud_rest_context rest_ctx = {
			.connect_socket = -1,
			.keep_alive = false,
			.timeout_ms = NRF_CLOUD_REST_TIMEOUT_NONE,
			.auth = NULL, /* AUTOGEN_JWT */
			.rx_buf = rx_buf,
			.rx_buf_len = sizeof(rx_buf),
			.fragment_size = 0,
			.status = 0,
			.response = NULL,
			.response_len = 0,
			.total_response_len = 0,
		};

		err = nrf_cloud_jwt_generate(0, jwt_buf, sizeof(jwt_buf));
		printk("AGNSS: JWT gen = %d\n", err);
		if (err == 0) {
			rest_ctx.auth = jwt_buf;
		}

		err = nrf_cloud_rest_agnss_data_get(&rest_ctx, &request, &result);
		if (err) {
			printk("AGNSS: data get failed, error %d\n", err);
			continue;
		}

		printk("AGNSS: got %d bytes, processing\n", result.agnss_sz);

		err = nrf_cloud_agnss_process(result.buf, result.agnss_sz);
		if (err) {
			printk("AGNSS: process failed, error %d\n", err);
			continue;
		}

		printk("AGNSS: ephemerides injected\n");
	}
}

K_THREAD_DEFINE(agnss_thread_id, AGNSS_THREAD_STACK, agnss_thread, NULL, NULL, NULL, 5, 0, 0);

void agnss_request(const struct nrf_modem_gnss_agnss_data_frame *req)
{
	pending_req = *req;
	k_sem_give(&agnss_sem);
}

void agnss_request_force(void)
{
	memset(&pending_req, 0, sizeof(pending_req));
	pending_req.data_flags = NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST |
				 NRF_MODEM_GNSS_AGNSS_POSITION_REQUEST;
	pending_req.system_count = 1;
	pending_req.system[0].system_id = NRF_MODEM_GNSS_SYSTEM_GPS;
	pending_req.system[0].sv_mask_ephe = 0xFFFFFFFFFFFFFFFFULL;
	k_sem_give(&agnss_sem);
}

int agnss_init(void)
{
	return 0;
}
