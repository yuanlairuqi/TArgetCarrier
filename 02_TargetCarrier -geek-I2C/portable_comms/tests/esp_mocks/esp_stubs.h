
#ifndef ESP_STUBS_H
#define ESP_STUBS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define UART_NUM_1 1
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define GPIO_MODE_INPUT 0
#define GPIO_PULLDOWN_ENABLE 1
#define SPI2_HOST 2
#define SPI_DMA_CH_AUTO 3
#define I2C_NUM_0 0
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 7
#define DMA_ATTR
typedef int esp_err_t;
typedef void *spi_device_handle_t;typedef void *i2c_master_bus_handle_t;typedef void *i2c_master_dev_handle_t;
typedef struct {int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk;} uart_config_t;
typedef struct {uint64_t pin_bit_mask;int mode,pull_down_en;} gpio_config_t;
typedef struct {int mosi_io_num,miso_io_num,sclk_io_num,quadwp_io_num,quadhd_io_num,max_transfer_sz;} spi_bus_config_t;
typedef struct {int clock_speed_hz,mode,spics_io_num,queue_size,cs_ena_posttrans;} spi_device_interface_config_t;
typedef struct {unsigned length,rxlength;const void *tx_buffer;void *rx_buffer;} spi_transaction_t;
typedef struct {int i2c_port,sda_io_num,scl_io_num,clk_source,glitch_ignore_cnt;} i2c_master_bus_config_t;
typedef struct {int dev_addr_length,device_address,scl_speed_hz;} i2c_device_config_t;
static int mock_ready=1,mock_fail,mock_uart_count,mock_writes,mock_ack,mock_spi;
static uint8_t mock_rx[256],mock_tx[128];
static int uart_driver_install(int a,int b,int c,int d,void*e,int f){return 0;}
static int uart_param_config(int a,const uart_config_t*b){return 0;}
static int uart_set_pin(int a,int b,int c,int d,int e){return 0;}
static int gpio_config(const gpio_config_t*a){return 0;}
static int gpio_get_level(int pin){return mock_ready;}
static int spi_bus_initialize(int a,const spi_bus_config_t*b,int c){return 0;}
static int spi_bus_add_device(int a,const spi_device_interface_config_t*b,spi_device_handle_t*c){*c=(void*)1;return 0;}
static int i2c_new_master_bus(const i2c_master_bus_config_t*a,i2c_master_bus_handle_t*b){*b=(void*)1;return 0;}
static int i2c_master_bus_add_device(i2c_master_bus_handle_t a,const i2c_device_config_t*b,i2c_master_dev_handle_t*c){*c=(void*)1;return 0;}
static int uart_read_bytes(int a,void*b,unsigned n,int t){int count=mock_uart_count;if(count){memcpy(b,mock_rx,count);mock_uart_count=0;}return count;}
static int uart_wait_tx_done(int a,int b){return 0;}
static int uart_write_bytes(int a,const void*b,unsigned n){memcpy(mock_tx,b,n);mock_writes++;return n;}
static int spi_device_transmit(spi_device_handle_t a,spi_transaction_t*t){mock_spi++;if(mock_fail)return ESP_FAIL;if(t->length!=1024||t->rxlength!=1024)return ESP_FAIL;memcpy(mock_tx,t->tx_buffer,128);memcpy(t->rx_buffer,mock_rx,128);return 0;}
static int i2c_master_transmit(i2c_master_dev_handle_t a,const void*b,unsigned n,int timeout){if(mock_fail)return ESP_FAIL;if(n==12){mock_ack++;if(memcmp(b,"AK\x02\0",4))return ESP_FAIL;}else{mock_writes++;memcpy(mock_tx,b,128);}return 0;}
static int i2c_master_receive(i2c_master_dev_handle_t a,void*b,unsigned n,int timeout){if(mock_fail)return ESP_FAIL;memcpy(b,mock_rx,n);return 0;}
#endif
