#pragma once

#include <stdint.h>

void saadc_init(void);

/* Sample pselp analog input. Returns raw 12-bit value (≥0) or -1 on timeout.
 * SAADC stays enabled between calls; timeout triggers a reset+re-enable. */
int saadc_read(uint32_t pselp);
