#ifndef GNSS_H
#define GNSS_H

#include <stdbool.h>

int gnss_init(void);
bool gnss_position_get(double *latitude, double *longitude, float *accuracy);

#endif /* GNSS_H */
