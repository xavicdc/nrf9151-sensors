#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <nrf_modem_gnss.h>

#include "gnss.h"

static volatile bool gnss_fix_valid;
static bool gnss_started;
static double last_latitude;
static double last_longitude;
static float last_altitude;
static float last_accuracy;

static void gnss_event_handler(int event)
{
	static struct nrf_modem_gnss_pvt_data_frame pvt;
	static uint32_t pvt_count;
	static bool prev_valid;

	if (event != NRF_MODEM_GNSS_EVT_PVT) {
		return;
	}

	if (nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT) != 0) {
		return;
	}

	uint8_t seen = 0;
	uint16_t max_cn0 = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (pvt.sv[i].cn0 > 0) {
			seen++;
			if (pvt.sv[i].cn0 > max_cn0) {
				max_cn0 = pvt.sv[i].cn0;
			}
		}
	}

	pvt_count++;

	if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		last_latitude = pvt.latitude;
		last_longitude = pvt.longitude;
		last_altitude = pvt.altitude;
		last_accuracy = pvt.accuracy;
		gnss_fix_valid = true;

		if (!prev_valid) {
			printk("GNSS fix obtained (%d sats)\n", seen);
		}
		prev_valid = true;
	} else {
		gnss_fix_valid = false;

		if (prev_valid) {
			printk("GNSS fix lost\n");
		}
		prev_valid = false;

		if ((pvt_count % 10) == 0) {
			printk("GNSS: searching, %d sats seen (max cn0 %d)\n",
			       seen, max_cn0);
		}
	}
}

int gnss_init(void)
{
	if (gnss_started) {
		return 0;
	}

	if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0) {
		printk("GNSS: failed to set event handler\n");
		return -1;
	}

	if (nrf_modem_gnss_start() != 0) {
		printk("GNSS: failed to start\n");
		return -1;
	}

	gnss_started = true;
	printk("GNSS started\n");
	return 0;
}

void gnss_reset(void)
{
	gnss_fix_valid = false;
}

void gnss_stop(void)
{
	nrf_modem_gnss_stop();
}

void gnss_start(void)
{
	if (!gnss_started) {
		gnss_init();
		return;
	}

	if (nrf_modem_gnss_start() != 0) {
		printk("GNSS: failed to (re)start\n");
	}
}

bool gnss_position_get(double *latitude, double *longitude, float *altitude,
		       float *accuracy)
{
	if (!gnss_fix_valid) {
		return false;
	}

	if (latitude) {
		*latitude = last_latitude;
	}
	if (longitude) {
		*longitude = last_longitude;
	}
	if (altitude) {
		*altitude = last_altitude;
	}
	if (accuracy) {
		*accuracy = last_accuracy;
	}

	return true;
}
