#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <nrf_modem_gnss.h>

#include "gnss.h"

static volatile bool gnss_fix_valid;
static bool gnss_started;
static double last_latitude;
static double last_longitude;
static float last_accuracy;

static void gnss_event_handler(int event)
{
	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT: {
		struct nrf_modem_gnss_pvt_data_frame pvt;

		if (nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT) == 0) {
			if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
				last_latitude = pvt.latitude;
				last_longitude = pvt.longitude;
				last_accuracy = pvt.accuracy;
				gnss_fix_valid = true;
				printk("GNSS fix: %.6f, %.6f (acc %.1f m)\n",
				       (double)pvt.latitude, (double)pvt.longitude,
				       (double)pvt.accuracy);
			} else {
				printk("GNSS: searching satellites...\n");
			}
		}
		break;
	}
	default:
		break;
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

bool gnss_position_get(double *latitude, double *longitude, float *accuracy)
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
	if (accuracy) {
		*accuracy = last_accuracy;
	}

	return true;
}
