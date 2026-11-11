#ifndef MOCK_FAT_H
#define MOCK_FAT_H
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
typedef struct sdmmc_card_t {int unused;} sdmmc_card_t;
typedef struct {bool format_if_mount_failed;int max_files,allocation_unit_size;} esp_vfs_fat_sdmmc_mount_config_t;
esp_err_t esp_vfs_fat_info(const char*,uint64_t*,uint64_t*);
esp_err_t esp_vfs_fat_sdspi_mount(const char*,const void*,const void*,const esp_vfs_fat_sdmmc_mount_config_t*,sdmmc_card_t**);
esp_err_t esp_vfs_fat_sdcard_unmount(const char*,sdmmc_card_t*);
#endif
