#include "sd_geek.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
/* Waveshare GEEK V1.1 on-board TF slot. LCD owns SPI2 and GPIO 7-12. */
#define CAP_SD_HOST SPI3_HOST
#define CAP_SD_MOUNT "/capture_sd"
static sdmmc_card_t *card;
static bool bus_owned;
esp_err_t cap_sd_mount(void){
 if(card||bus_owned)return ESP_ERR_INVALID_STATE;
 spi_bus_config_t bus={.mosi_io_num=35,.miso_io_num=37,.sclk_io_num=36,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096};
 esp_err_t err=spi_bus_initialize(CAP_SD_HOST,&bus,SPI_DMA_CH_AUTO);if(err!=ESP_OK)return err;bus_owned=true;
 sdmmc_host_t host=SDSPI_HOST_DEFAULT();host.slot=CAP_SD_HOST;host.max_freq_khz=10000;
 sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();slot.host_id=CAP_SD_HOST;slot.gpio_cs=34;
 esp_vfs_fat_sdmmc_mount_config_t cfg={.format_if_mount_failed=false,.max_files=4,.allocation_unit_size=16*1024};
 err=esp_vfs_fat_sdspi_mount(CAP_SD_MOUNT,&host,&slot,&cfg,&card);
 if(err!=ESP_OK){card=0;if(spi_bus_free(CAP_SD_HOST)==ESP_OK)bus_owned=false;}
 return err;
}
esp_err_t cap_sd_unmount(void){
 /* Idempotent cleanup also handles a bus left owned after a partial failure. */
 if(card){
  esp_err_t err=esp_vfs_fat_sdcard_unmount(CAP_SD_MOUNT,card);
  if(err!=ESP_OK)return err;
  card=0;
 }
 if(bus_owned){
  esp_err_t err=spi_bus_free(CAP_SD_HOST);if(err!=ESP_OK)return err;
  bus_owned=false;
 }
 return ESP_OK;
}
