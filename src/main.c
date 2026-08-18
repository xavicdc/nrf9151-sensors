#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <math.h>

#include "qmp6988.h"
#include "mqtt_app.h"
#include "gnss.h"
#include "agnss.h"
#include "net_config.h"

#define I2C_NODE DT_NODELABEL(i2c2)
#define SHT30_NODE DT_NODELABEL(sht30)

#define NUM_LEDS 4

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)
#define LED3_NODE DT_ALIAS(led3)

#define SW0_NODE DT_ALIAS(sw0)
#define SW1_NODE DT_ALIAS(sw1)
#define SW2_NODE DT_ALIAS(sw2)
#define SW3_NODE DT_ALIAS(sw3)

static const struct gpio_dt_spec leds[NUM_LEDS] = {
	GPIO_DT_SPEC_GET(LED0_NODE, gpios),
	GPIO_DT_SPEC_GET(LED1_NODE, gpios),
	GPIO_DT_SPEC_GET(LED2_NODE, gpios),
	GPIO_DT_SPEC_GET(LED3_NODE, gpios),
};

static const struct gpio_dt_spec buttons[NUM_LEDS] = {
	GPIO_DT_SPEC_GET(SW0_NODE, gpios),
	GPIO_DT_SPEC_GET(SW1_NODE, gpios),
	GPIO_DT_SPEC_GET(SW2_NODE, gpios),
	GPIO_DT_SPEC_GET(SW3_NODE, gpios),
};

static const uint32_t blink_periods_ms[NUM_LEDS] = {
	500, 1000, 1500, 2000,
};

static bool led_enabled[NUM_LEDS] = { true, true, true, true };

static struct gpio_callback button_cb[NUM_LEDS];
static struct k_work_delayable led_work[NUM_LEDS];
static struct k_work_delayable debounce_work[NUM_LEDS];

/* Anonimització de la posició enviada a MQTT:
 * es desplaça la coordenada amb una distància i direcció aleatòries
 * (200-600 m) a cada enviament, de manera que la posició publicada
 * no sigui la real i sembli mòbil.
 */
#define ANON_MIN_DIST_M 200.0
#define ANON_MAX_DIST_M 600.0
#define METERS_PER_DEG_LAT 111320.0
#define PI 3.14159265358979323846

static uint32_t anon_rng_state;

static uint32_t anon_rand(void)
{
	/* xorshift32: senzill i suficient per desplaçar la posició. */
	uint32_t x = anon_rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	anon_rng_state = x;

	return x;
}

static void anon_offset_m(double lat, double *dlat, double *dlon)
{
	double dist = ANON_MIN_DIST_M +
		      (anon_rand() % (uint32_t)(ANON_MAX_DIST_M - ANON_MIN_DIST_M + 1));
	double angle = (anon_rand() % 628) / 100.0; /* [0, 2π) */
	double lat_rad = lat * PI / 180.0;

	*dlat = dist * cos(angle) / METERS_PER_DEG_LAT;
	*dlon = dist * sin(angle) / (METERS_PER_DEG_LAT * cos(lat_rad));
}

static void led_toggle_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	int idx = dwork - led_work;

	if (!led_enabled[idx]) {
		return;
	}

	gpio_pin_toggle_dt(&leds[idx]);
	k_work_reschedule(dwork, K_MSEC(blink_periods_ms[idx] / 2));
}

static void button_process_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	int idx = dwork - debounce_work;

	led_enabled[idx] = !led_enabled[idx];

	if (led_enabled[idx]) {
		gpio_pin_set_dt(&leds[idx], 1);
		k_work_reschedule(&led_work[idx], K_MSEC(blink_periods_ms[idx] / 2));
	} else {
		k_work_cancel_delayable(&led_work[idx]);
		gpio_pin_set_dt(&leds[idx], 0);
	}
	printk("LED%u %s\n", idx, led_enabled[idx] ? "ON" : "OFF");
}

static void button_isr(const struct device *port, struct gpio_callback *cb,
		       gpio_port_pins_t pins)
{
	for (int i = 0; i < NUM_LEDS; i++) {
		if (cb == &button_cb[i]) {
			k_work_reschedule(&debounce_work[i], K_MSEC(50));
			break;
		}
	}
}

static void print_sensor_value(const char *label, struct sensor_value *val)
{
	printk("%s %d.%06d", label, val->val1, val->val2);
}

static void sht30_probe(const struct device *i2c_dev)
{
	uint8_t soft_reset[2] = { 0x30, 0xA2 };
	uint8_t status_cmd[2] = { 0xF3, 0x2D };
	uint8_t rx[3];

	for (int addr = 0x44; addr <= 0x45; addr++) {
		if (i2c_write(i2c_dev, soft_reset, sizeof(soft_reset), addr) != 0) {
			printk("SHT30 probe @0x%02x: no ACK\n", addr);
			continue;
		}
		printk("SHT30 probe @0x%02x: ACK after soft reset\n", addr);
		k_msleep(20);
		if (i2c_write_read(i2c_dev, addr, status_cmd, sizeof(status_cmd),
				   rx, sizeof(rx)) == 0) {
			printk("  status reg: %02x %02x %02x\n", rx[0], rx[1], rx[2]);
		} else {
			printk("  status read FAIL\n");
		}
	}
}

static void i2c_scan(const struct device *i2c_dev)
{
	uint8_t addr;

	printk("Scanning I2C bus (full range)...\n");
	for (addr = 0x03; addr <= 0x7f; addr++) {
		struct i2c_msg msgs[1];
		uint8_t buf = 0;

		msgs[0].buf = &buf;
		msgs[0].len = 1;
		msgs[0].flags = I2C_MSG_WRITE | I2C_MSG_STOP;

		if (i2c_transfer(i2c_dev, msgs, 1, addr) == 0) {
			printk("Device found at 0x%02x\n", addr);
		}
	}
	printk("Scan done\n");
}

int main(void)
{
	const struct device *i2c_dev = DEVICE_DT_GET(I2C_NODE);
	const struct device *sht30 = DEVICE_DT_GET(SHT30_NODE);

	printk("nRF9151-SMA-DK sensors + LED/button control + MQTT\n");

	anon_rng_state = (uint32_t)k_uptime_get() ^ 0x9E3779B9U;

	mqtt_app_init();
	agnss_init();

	for (int i = 0; i < 24; i++) {
		if (gnss_init() == 0) {
			break;
		}
		k_sleep(K_SECONDS(5));
	}

	for (int i = 0; i < NUM_LEDS; i++) {
		if (!device_is_ready(leds[i].port) || !device_is_ready(buttons[i].port)) {
			printk("Error: LED/button %d device not ready\n", i);
			return 0;
		}
	}

	for (int i = 0; i < NUM_LEDS; i++) {
		gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_ACTIVE);
		gpio_pin_set_dt(&leds[i], 1);

		k_work_init_delayable(&led_work[i], led_toggle_work);
		k_work_reschedule(&led_work[i], K_MSEC(blink_periods_ms[i] / 2));

		gpio_pin_configure_dt(&buttons[i], GPIO_INPUT);
		gpio_pin_interrupt_configure_dt(&buttons[i], GPIO_INT_EDGE_TO_ACTIVE);

		gpio_init_callback(&button_cb[i], button_isr, BIT(buttons[i].pin));
		gpio_add_callback(buttons[i].port, &button_cb[i]);

		k_work_init_delayable(&debounce_work[i], button_process_work);
	}

	if (!device_is_ready(i2c_dev)) {
		printk("Error: I2C2 not ready\n");
		return 0;
	}

	i2c_scan(i2c_dev);

	sht30_probe(i2c_dev);

	if (!device_is_ready(sht30)) {
		printk("Warning: SHT30 not ready - humidity unavailable\n");
	}

	if (!qmp6988_init(i2c_dev)) {
		printk("Error: QMP6988 not found\n");
		return 0;
	}
	printk("QMP6988 initialized\n");

	while (1) {
		struct sensor_value temp, hum;
		int32_t pressure_pa, temp_mdeg;
		bool sht30_ok = false;
		bool qmp_ok = false;
		char payload[160];
		static int64_t last_acquire_ms = 0;

		gnss_init();

		if (last_acquire_ms < 0 ||
		    (k_uptime_get() - last_acquire_ms) >=
			    (GNSS_ACQUIRE_INTERVAL_SECONDS * 1000)) {
			mqtt_app_gnss_acquire(GNSS_ACQUIRE_TIMEOUT_SECONDS);
			last_acquire_ms = k_uptime_get();
		}

		if (qmp6988_read(i2c_dev, &pressure_pa, &temp_mdeg) == 0) {
			qmp_ok = true;
			printk("QMP6988 temp=%d.%03d C  pressure=%d.%02d hPa\n",
			       temp_mdeg / 1000, abs(temp_mdeg) % 1000,
			       pressure_pa / 100, abs(pressure_pa) % 100);
		} else {
			printk("QMP6988 read error\n");
		}

		if (device_is_ready(sht30) &&
		    sensor_sample_fetch(sht30) == 0 &&
		    sensor_channel_get(sht30, SENSOR_CHAN_AMBIENT_TEMP, &temp) == 0 &&
		    sensor_channel_get(sht30, SENSOR_CHAN_HUMIDITY, &hum) == 0) {
			sht30_ok = true;
			printk("SHT30 temp=");
			print_sensor_value("", &temp);
			printk(" C  hum=");
			print_sensor_value("", &hum);
			printk(" %%\n");
		} else {
			printk("SHT30 read error\n");
		}

		if (qmp_ok) {
			int n = snprintk(payload, sizeof(payload),
				 "{\"temperature\":%d.%d,\"pressure\":%d",
				 temp_mdeg / 1000, (abs(temp_mdeg) % 1000) / 100,
				 pressure_pa / 100);

			double lat, lon;
			float alt, acc;

			if (gnss_position_get(&lat, &lon, &alt, &acc)) {
				int lat_deg = (int)lat;
				int lat_frac = (int)((lat - lat_deg) * 10000.0);
				int lon_deg = (int)lon;
				int lon_frac = (int)((lon - lon_deg) * 10000.0);
				int alt_int = (int)alt;
				int alt_frac = (int)((alt - alt_int) * 10.0);

				if (lat_frac < 0) {
					lat_frac = -lat_frac;
				}
				if (lon_frac < 0) {
					lon_frac = -lon_frac;
				}
				if (alt_frac < 0) {
					alt_frac = -alt_frac;
				}

				printk("GPS fix: %d.%04d, %d.%04d (alt %d.%d m, acc %d m)\n",
				       lat_deg, lat_frac, lon_deg, lon_frac,
				       alt_int, alt_frac, (int)acc);

				/* Anonimitza per a MQTT: desplaça la coordenada amb un
				 * offset aleatori de 200-600 m perquè la posició
				 * publicada no sigui la real i sembli mòbil.
				 */
				double dlat, dlon;
				double lat_anon, lon_anon;

				anon_offset_m(lat, &dlat, &dlon);
				lat_anon = lat + dlat;
				lon_anon = lon + dlon;

				lat_deg = (int)lat_anon;
				lat_frac = (int)((lat_anon - lat_deg) * 10000.0);
				lon_deg = (int)lon_anon;
				lon_frac = (int)((lon_anon - lon_deg) * 10000.0);

				if (lat_frac < 0) {
					lat_frac = -lat_frac;
				}
				if (lon_frac < 0) {
					lon_frac = -lon_frac;
				}

				n = strlen(payload);
				snprintk(payload + n, sizeof(payload) - n,
					 ",\"latitude\":%d.%04d,\"longitude\":%d.%04d,\"altitude\":%d.%d",
					 lat_deg, lat_frac, lon_deg, lon_frac,
					 alt_int, alt_frac);
			}

			if (sht30_ok) {
				n = strlen(payload);
				snprintk(payload + n, sizeof(payload) - n,
					 ",\"humidity\":%d.%d", hum.val1,
					 hum.val2 / 100000);
			} else {
				n = strlen(payload);
				snprintk(payload + n, sizeof(payload) - n, ",\"humidity\":null");
			}

			n = strlen(payload);
			snprintk(payload + n, sizeof(payload) - n,
				 ",\"buttons\":[%d,%d,%d,%d],\"leds\":[%d,%d,%d,%d]",
				 gpio_pin_get_dt(&buttons[0]), gpio_pin_get_dt(&buttons[1]),
				 gpio_pin_get_dt(&buttons[2]), gpio_pin_get_dt(&buttons[3]),
				 led_enabled[0] ? 1 : 0, led_enabled[1] ? 1 : 0,
				 led_enabled[2] ? 1 : 0, led_enabled[3] ? 1 : 0);

			n = strlen(payload);
			snprintk(payload + n, sizeof(payload) - n, "}");

			printk("PAYLOAD: %s\n", payload);
			mqtt_app_publish(payload);
		}

		k_sleep(K_SECONDS(IS_ENABLED(MQTT_USE_NTN_NBIOT)
				     ? MQTT_PUBLISH_INTERVAL_SECONDS_NTN
				     : MQTT_PUBLISH_INTERVAL_SECONDS));
	}

	return 0;
}
