#ifndef AGNSS_H
#define AGNSS_H

#include <nrf_modem_gnss.h>

int agnss_init(void);
void agnss_request(const struct nrf_modem_gnss_agnss_data_frame *req);
void agnss_request_force(void);

#endif /* AGNSS_H */
