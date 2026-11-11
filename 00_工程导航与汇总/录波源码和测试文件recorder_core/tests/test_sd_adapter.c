#include <assert.h>
#include <stdio.h>
#include "../sd_geek.c"
static unsigned init_count,mount_count,unmount_count,free_count;
static bool free_fail,mount_fail,unmount_fail;static sdmmc_card_t fake;
esp_err_t spi_bus_initialize(int host,const spi_bus_config_t*b,int dma){(void)dma;assert(host==SPI3_HOST&&b->sclk_io_num==36&&b->mosi_io_num==35&&b->miso_io_num==37);init_count++;return 0;}
esp_err_t spi_bus_free(int host){assert(host==SPI3_HOST);free_count++;return free_fail?1:0;}
esp_err_t esp_vfs_fat_sdspi_mount(const char*p,const void*h,const void*d,const esp_vfs_fat_sdmmc_mount_config_t*c,sdmmc_card_t**out){(void)p;(void)h;(void)d;assert(!c->format_if_mount_failed);mount_count++;if(mount_fail)return 1;*out=&fake;return 0;}
esp_err_t esp_vfs_fat_sdcard_unmount(const char*p,sdmmc_card_t*c){(void)p;assert(c==&fake);unmount_count++;return unmount_fail?1:0;}
int main(void){
 assert(cap_sd_unmount()==0&&free_count==0&&unmount_count==0);
 assert(cap_sd_mount()==0&&card&&bus_owned);
 unmount_fail=true;assert(cap_sd_unmount()!=0&&card&&bus_owned&&free_count==0);
 unmount_fail=false;free_fail=true;assert(cap_sd_unmount()!=0&&!card&&bus_owned);
 unsigned prior_unmount=unmount_count;free_fail=false;assert(cap_sd_unmount()==0&&!card&&!bus_owned&&unmount_count==prior_unmount);
 mount_fail=true;free_fail=true;assert(cap_sd_mount()!=0&&!card&&bus_owned);
 free_fail=false;assert(cap_sd_unmount()==0&&!bus_owned);
 mount_fail=false;assert(cap_sd_mount()==0);assert(cap_sd_unmount()==0);assert(init_count==3&&mount_count==3);
 puts("SD adapter PASS: idempotent release, unmount/free failure retry, failed mount cleanup, no format");return 0;
}
