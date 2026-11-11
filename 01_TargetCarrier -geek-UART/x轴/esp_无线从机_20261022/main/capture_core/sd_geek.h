#ifndef CAP_SD_GEEK_H
#define CAP_SD_GEEK_H
#include "esp_err.h"
/* All calls must run on the storage task, never on radio/control callbacks. */
esp_err_t cap_sd_mount(void);
esp_err_t cap_sd_unmount(void);
#endif
