#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "qmp6988.h"

typedef int16_t S16;
typedef uint16_t U16;
typedef int32_t S32;
typedef uint32_t U32;
typedef int64_t S64;

#define SUBTRACTOR 8388608

typedef struct {
	S32 COE_a0;
	S16 COE_a1;
	S16 COE_a2;
	S32 COE_b00;
	S16 COE_bt1;
	S16 COE_bt2;
	S16 COE_bp1;
	S16 COE_b11;
	S16 COE_bp2;
	S16 COE_b12;
	S16 COE_b21;
	S16 COE_bp3;
} qmp6988_cali_data_t;

typedef struct {
	S32 a0, b00;
	S32 a1, a2;
	S64 bt1, bt2, bp1, b11, bp2, b12, b21, bp3;
} qmp6988_ik_data_t;

static qmp6988_ik_data_t ik;

static int qmp6988_write_reg(const struct device *i2c_dev, uint8_t reg,
			     uint8_t val)
{
	uint8_t buf[2] = { reg, val };

	return i2c_write(i2c_dev, buf, sizeof(buf), QMP6988_ADDR);
}

static int qmp6988_read_bytes(const struct device *i2c_dev, uint8_t reg,
			      uint8_t *buf, uint8_t len)
{
	return i2c_write_read(i2c_dev, QMP6988_ADDR, &reg, 1, buf, len);
}

static void qmp6988_get_calibration(const struct device *i2c_dev)
{
	uint8_t tr[QMP6988_CALIBRATION_LEN] = { 0 };

	qmp6988_read_bytes(i2c_dev, QMP6988_CALIBRATION_START, tr,
			   QMP6988_CALIBRATION_LEN);

	ik.a0 = (S32)((((S32)(tr[18]) << 12) | ((S32)(tr[19]) << 4) |
		       (tr[24] & 0x0f))
		      << 12) >>
		12;
	ik.a1 = (S16)((tr[20] << 8) | tr[21]);
	ik.a2 = (S16)((tr[22] << 8) | tr[23]);

	ik.b00 = (S32)((((S32)(tr[0]) << 12) | ((S32)(tr[1]) << 4) |
			((tr[24] & 0xf0) >> 4))
		       << 12) >>
		 12;
	ik.bt1 = (S16)((tr[2] << 8) | tr[3]);
	ik.bt2 = (S16)((tr[4] << 8) | tr[5]);
	ik.bp1 = (S16)((tr[6] << 8) | tr[7]);
	ik.b11 = (S16)((tr[8] << 8) | tr[9]);
	ik.bp2 = (S16)((tr[10] << 8) | tr[11]);
	ik.b12 = (S16)((tr[12] << 8) | tr[13]);
	ik.b21 = (S16)((tr[14] << 8) | tr[15]);
	ik.bp3 = (S16)((tr[16] << 8) | tr[17]);

	ik.a1 = 3608L * (S32)ik.a1 - 1731677965L;
	ik.a2 = 16889L * (S32)ik.a2 - 87619360L;

	ik.bt1 = 2982L * (S64)ik.bt1 + 107370906L;
	ik.bt2 = 329854L * (S64)ik.bt2 + 108083093L;
	ik.bp1 = 19923L * (S64)ik.bp1 + 1133836764L;
	ik.b11 = 2406L * (S64)ik.b11 + 118215883L;
	ik.bp2 = 3079L * (S64)ik.bp2 - 181579595L;
	ik.b12 = 6846L * (S64)ik.b12 + 85590281L;
	ik.b21 = 13836L * (S64)ik.b21 + 79333336L;
	ik.bp3 = 2915L * (S64)ik.bp3 + 157155561L;
}

static S16 qmp6988_conv_temperature(S32 dt)
{
	S64 wk1, wk2;

	wk1 = (S64)ik.a1 * (S64)dt;
	wk2 = ((S64)ik.a2 * (S64)dt) >> 14;
	wk2 = (wk2 * (S64)dt) >> 10;
	wk2 = ((wk1 + wk2) / 32767) >> 19;

	return (S16)((ik.a0 + wk2) >> 4);
}

static S32 qmp6988_get_pressure(S32 dp, S16 tx)
{
	S64 wk1, wk2, wk3;

	wk1 = ((S64)ik.bt1 * (S64)tx);
	wk2 = ((S64)ik.bp1 * (S64)dp) >> 5;
	wk1 += wk2;

	wk2 = ((S64)ik.bt2 * (S64)tx) >> 1;
	wk2 = (wk2 * (S64)tx) >> 8;
	wk3 = wk2;

	wk2 = ((S64)ik.b11 * (S64)tx) >> 4;
	wk2 = (wk2 * (S64)dp) >> 1;
	wk3 += wk2;

	wk2 = ((S64)ik.bp2 * (S64)dp) >> 13;
	wk2 = (wk2 * (S64)dp) >> 1;
	wk3 += wk2;

	wk1 += wk3 >> 14;

	wk2 = ((S64)ik.b12 * (S64)tx);
	wk2 = (wk2 * (S64)tx) >> 22;
	wk2 = (wk2 * (S64)dp) >> 1;
	wk3 = wk2;

	wk2 = ((S64)ik.b21 * (S64)tx) >> 6;
	wk2 = (wk2 * (S64)dp) >> 23;
	wk2 = (wk2 * (S64)dp) >> 1;
	wk3 += wk2;

	wk2 = ((S64)ik.bp3 * (S64)dp) >> 12;
	wk2 = (wk2 * (S64)dp) >> 23;
	wk2 = (wk2 * (S64)dp);
	wk3 += wk2;

	wk1 += wk3 >> 15;
	wk1 /= 32767L;
	wk1 >>= 11;
	wk1 += ik.b00;

	return (S32)wk1;
}

bool qmp6988_init(const struct device *i2c_dev)
{
	uint8_t chip_id;
	uint8_t ctrl;
	int err;

	err = qmp6988_read_bytes(i2c_dev, QMP6988_CHIP_ID_REG, &chip_id, 1);
	if (err != 0 || chip_id != QMP6988_CHIP_ID) {
		return false;
	}

	qmp6988_write_reg(i2c_dev, QMP6988_RESET_REG, 0xE6);
	k_msleep(20);
	qmp6988_write_reg(i2c_dev, QMP6988_RESET_REG, 0x00);

	qmp6988_get_calibration(i2c_dev);

	qmp6988_read_bytes(i2c_dev, QMP6988_CTRLMEAS_REG, &ctrl, 1);
	ctrl = (ctrl & 0xfc) | QMP6988_NORMAL_MODE;
	qmp6988_write_reg(i2c_dev, QMP6988_CTRLMEAS_REG, ctrl);

	qmp6988_write_reg(i2c_dev, QMP6988_CONFIG_REG, 0x02);

	qmp6988_read_bytes(i2c_dev, QMP6988_CTRLMEAS_REG, &ctrl, 1);
	ctrl = (ctrl & 0xe3) | (0x04 << 2);
	qmp6988_write_reg(i2c_dev, QMP6988_CTRLMEAS_REG, ctrl);

	qmp6988_read_bytes(i2c_dev, QMP6988_CTRLMEAS_REG, &ctrl, 1);
	ctrl = (ctrl & 0x1f) | (0x01 << 5);
	qmp6988_write_reg(i2c_dev, QMP6988_CTRLMEAS_REG, ctrl);

	k_msleep(50);

	return true;
}

int qmp6988_read(const struct device *i2c_dev, int32_t *pressure_pa,
		 int32_t *temp_mdeg)
{
	uint8_t tr[6] = { 0 };
	U32 p_read, t_read;
	S32 p_raw, t_raw;
	S16 t_int;
	S32 p_int;

	int err = qmp6988_read_bytes(i2c_dev, QMP6988_PRESSURE_MSB_REG, tr, 6);
	if (err != 0) {
		return err;
	}

	p_read = (U32)(((U32)tr[0] << 16) | ((U16)tr[1] << 8) | tr[2]);
	t_read = (U32)(((U32)tr[3] << 16) | ((U16)tr[4] << 8) | tr[5]);
	p_raw = (S32)(p_read - SUBTRACTOR);
	t_raw = (S32)(t_read - SUBTRACTOR);

	t_int = qmp6988_conv_temperature(t_raw);
	p_int = qmp6988_get_pressure(p_raw, t_int);

	if (temp_mdeg) {
		*temp_mdeg = (int32_t)t_int * 1000 / 256;
	}
	if (pressure_pa) {
		*pressure_pa = (int32_t)p_int / 16;
	}

	return 0;
}
