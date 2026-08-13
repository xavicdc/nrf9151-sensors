#ifndef GNSS_H
#define GNSS_H

#include <stdbool.h>

int gnss_init(void);
void gnss_reset(void);
void gnss_stop(void);
void gnss_start(void);
bool gnss_position_get(double *latitude, double *longitude, float *altitude,
		       float *accuracy);

#endif /* GNSS_H */
