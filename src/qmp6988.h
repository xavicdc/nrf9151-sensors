#ifndef QMP6988_H
#define QMP6988_H

#include <stdint.h>
#include <stdbool.h>

#define QMP6988_ADDR 0x70
#define QMP6988_CHIP_ID 0x5C

#define QMP6988_CHIP_ID_REG     0xD1
#define QMP6988_RESET_REG       0xE0
#define QMP6988_CTRLMEAS_REG    0xF4
#define QMP6988_CONFIG_REG      0xF1
#define QMP6988_PRESSURE_MSB_REG 0xF7
#define QMP6988_CALIBRATION_START 0xA0
#define QMP6988_CALIBRATION_LEN 25

#define QMP6988_NORMAL_MODE 0x03

bool qmp6988_init(const struct device *i2c_dev);
int qmp6988_read(const struct device *i2c_dev, int32_t *pressure_pa,
		 int32_t *temp_mdeg);

#endif
