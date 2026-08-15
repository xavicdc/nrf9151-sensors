/*
 * Assistència A-GNSS "minimal" adaptada del sample cellular/gnss del NCS:
 * - Escriu l'almanac de fàbrica al mòdem.
 * - Injecta l'hora actual (AT+CCLK) i la ubicació aproximada (MCC de la xarxa).
 * No requereix compte de núvol ni credencials.
 *
 * La injecció s'executa en un thread dedicat (les ordres AT bloquegen i no es
 * poden cridar des del context de l'event GNSS).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/timeutil.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>

#include "agnss.h"
#include "factory_almanac_v3.h"
#include "mcc_location_table.h"

#define GPS_TO_UNIX_UTC_OFFSET_SECONDS (315964800UL)
#define GPS_TO_UTC_LEAP_SECONDS (18UL)
#define PLMN_STR_MAX_LEN 8
#define AGNSS_THREAD_STACK 4096

K_SEM_DEFINE(agnss_sem, 0, 1);
static struct nrf_modem_gnss_agnss_data_frame pending_req;

static void factory_almanac_write(void)
{
	int err;

	err = nrf_modem_at_printf("AT%%XFILEWRITE=1,\"%s\",\"%s\"",
				  FACTORY_ALMANAC_DATA_V3,
				  FACTORY_ALMANAC_CHECKSUM_V3);
	if (err != 0) {
		printk("AGNSS: failed to write factory almanac\n");
		return;
	}

	printk("AGNSS: factory almanac written\n");
}

static int64_t utc_to_gps_sec(const int64_t utc_sec)
{
	return (utc_sec - GPS_TO_UNIX_UTC_OFFSET_SECONDS) + GPS_TO_UTC_LEAP_SECONDS;
}

static void gps_sec_to_day_time(int64_t gps_sec, uint16_t *gps_day, uint32_t *gps_time_of_day)
{
	*gps_day = (uint16_t)(gps_sec / SEC_PER_DAY);
	*gps_time_of_day = (uint32_t)(gps_sec % SEC_PER_DAY);
}

static void time_inject(void)
{
	int ret;
	struct tm date_time;
	int64_t utc_sec;
	int64_t gps_sec;
	struct nrf_modem_gnss_agnss_gps_data_system_time_and_sv_tow gps_time = { 0 };

	ret = nrf_modem_at_scanf("AT+CCLK?", "+CCLK: \"%u/%u/%u,%u:%u:%u",
				 &date_time.tm_year, &date_time.tm_mon,
				 &date_time.tm_mday, &date_time.tm_hour,
				 &date_time.tm_min, &date_time.tm_sec);
	if (ret != 6) {
		printk("AGNSS: could not read time from modem, skipping\n");
		return;
	}

	date_time.tm_year = date_time.tm_year + 2000 - 1900;
	date_time.tm_mon--;

	utc_sec = timeutil_timegm64(&date_time);
	gps_sec = utc_to_gps_sec(utc_sec);
	gps_sec_to_day_time(gps_sec, &gps_time.date_day, &gps_time.time_full_s);

	ret = nrf_modem_gnss_agnss_write(&gps_time, sizeof(gps_time),
					 NRF_MODEM_GNSS_AGNSS_GPS_SYSTEM_CLOCK_AND_TOWS);
	if (ret != 0) {
		printk("AGNSS: failed to inject time, error %d\n", ret);
		return;
	}

	printk("AGNSS: time injected (GPS day %u, ToD %u)\n",
	       gps_time.date_day, gps_time.time_full_s);
}

static void location_inject(void)
{
	int err;
	char plmn_str[PLMN_STR_MAX_LEN + 1];
	uint16_t mcc;
	const struct mcc_table *mcc_info;
	struct nrf_modem_gnss_agnss_data_location location = { 0 };

	err = nrf_modem_at_scanf("AT%XMONITOR",
				 "%%XMONITOR: "
				 "%*d"
				 ",%*[^,]"
				 ",%*[^,]"
				 ",%" STRINGIFY(PLMN_STR_MAX_LEN) "[^,]",
				 plmn_str);
	if (err != 1) {
		printk("AGNSS: could not read PLMN, skipping location\n");
		return;
	}

	plmn_str[4] = '\0';
	mcc = strtol(plmn_str + 1, NULL, 10);

	mcc_info = mcc_lookup(mcc);
	if (mcc_info == NULL) {
		printk("AGNSS: no location for MCC %u\n", mcc);
		return;
	}

	location.latitude = lat_convert(mcc_info->lat);
	location.longitude = lon_convert(mcc_info->lon);
	location.unc_semimajor = mcc_info->unc_semimajor;
	location.unc_semiminor = mcc_info->unc_semiminor;
	location.orientation_major = mcc_info->orientation;
	location.confidence = mcc_info->confidence;
	location.unc_altitude = 255;

	err = nrf_modem_gnss_agnss_write(&location, sizeof(location),
					 NRF_MODEM_GNSS_AGNSS_LOCATION);
	if (err) {
		printk("AGNSS: failed to inject location, error %d\n", err);
		return;
	}

	printk("AGNSS: location injected for MCC %u\n", mcc);
}

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
	k_sem_give(&agnss_sem);
}

static void agnss_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		static int64_t last_process_ms = -60000;

		k_sem_take(&agnss_sem, K_FOREVER);

		/* Rate-limit: avoid spamming AT commands if the GNSS cannot fix. */
		if ((k_uptime_get() - last_process_ms) < 60000) {
			continue;
		}
		last_process_ms = k_uptime_get();

		k_sleep(K_SECONDS(2));

		printk("AGNSS: processing assistance request\n");

		if (pending_req.data_flags & NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST) {
			time_inject();
		}

		if (pending_req.data_flags & NRF_MODEM_GNSS_AGNSS_POSITION_REQUEST) {
			location_inject();
		}
	}
}

K_THREAD_DEFINE(agnss_thread_id, AGNSS_THREAD_STACK, agnss_thread, NULL, NULL, NULL, 5, 0, 0);

int agnss_init(void)
{
	factory_almanac_write();

	return 0;
}
