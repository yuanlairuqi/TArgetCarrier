#include "pulse_capture.h"
#include "pulse_wire.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "driver/gpio.h"
#ifdef CONFIG_ROBOT_PULSE_CAPTURE_ENABLED
#include "driver/mcpwm_cap.h"
#endif
static const char *TAG="pulse_capture";
static pulse_snapshot_t snapshot;
static portMUX_TYPE capture_lock=portMUX_INITIALIZER_UNLOCKED;
static bool available,recording;
static uint16_t previous_conn=BLE_HS_CONN_HANDLE_NONE;
static size_t packet_index;
static int state; // 0 waiting, 1 capturing, 2 transferring
static int64_t started_us,next_us,transfer_us;
#ifdef CONFIG_ROBOT_PULSE_CAPTURE_ENABLED
static uint32_t start_tick;
static mcpwm_cap_timer_handle_t timer;
static mcpwm_cap_channel_handle_t channels[3];
static bool on_edge(mcpwm_cap_channel_handle_t handle,const mcpwm_capture_event_data_t *event,void *arg){
    (void)handle;unsigned channel=(unsigned)(uintptr_t)arg;
    portENTER_CRITICAL_ISR(&capture_lock);
    if(recording){
        // Unsigned subtraction handles the 32-bit hardware timer wrapping.
        uint32_t delta=event->cap_value-start_tick;
        if(delta<snapshot.duration){
            if(snapshot.count<PULSE_MAX_EDGES){
                pulse_edge_t *edge=&snapshot.edges[snapshot.count++];edge->tick=delta;
                edge->tag=(uint8_t)(channel|((event->cap_edge==MCPWM_CAP_EDGE_POS)?2U:0U));
            }else snapshot.flags|=PULSE_OVERFLOW;
        }
    }
    portEXIT_CRITICAL_ISR(&capture_lock);return false;
}
static esp_err_t setup(void){
    const int pins[2]={CONFIG_ROBOT_PULSE_PUL_GPIO,CONFIG_ROBOT_PULSE_DIR_GPIO};
    if(pins[0]==pins[1]||!GPIO_IS_VALID_GPIO(pins[0])||!GPIO_IS_VALID_GPIO(pins[1]))return ESP_ERR_INVALID_ARG;
#ifdef CONFIG_ROBOT_UART_ENABLED
    for(unsigned i=0;i<2;i++)if(pins[i]==CONFIG_ROBOT_UART_TX_GPIO||pins[i]==CONFIG_ROBOT_UART_RX_GPIO)return ESP_ERR_INVALID_ARG;
#endif
    gpio_config_t inputs={.pin_bit_mask=(1ULL<<pins[0])|(1ULL<<pins[1]),.mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE,.pull_down_en=GPIO_PULLDOWN_ENABLE,.intr_type=GPIO_INTR_DISABLE};
    esp_err_t err=gpio_config(&inputs);if(err!=ESP_OK)return err;
    mcpwm_capture_timer_config_t t={.group_id=0,.clk_src=MCPWM_CAPTURE_CLK_SRC_DEFAULT,.resolution_hz=0};
    err=mcpwm_new_capture_timer(&t,&timer);if(err!=ESP_OK)return err;
    for(unsigned i=0;i<3;i++){
        // Channel 2 is an internal clock marker; it has no GPIO or edge interrupts.
        mcpwm_capture_channel_config_t c={.gpio_num=i<2?pins[i]:-1,.prescale=1,
            .flags={.pos_edge=i<2,.neg_edge=i<2}};
        err=mcpwm_new_capture_channel(timer,&c,&channels[i]);if(err!=ESP_OK)return err;
        if(i<2){mcpwm_capture_event_callbacks_t cb={.on_cap=on_edge};
            err=mcpwm_capture_channel_register_event_callbacks(channels[i],&cb,(void *)(uintptr_t)i);if(err!=ESP_OK)return err;}
        err=mcpwm_capture_channel_enable(channels[i]);if(err!=ESP_OK)return err;
    }
    err=mcpwm_capture_timer_get_resolution(timer,&snapshot.hz);if(err!=ESP_OK)return err;
    snapshot.duration=snapshot.hz/10U;snapshot.pul_gpio=pins[0];snapshot.dir_gpio=pins[1];
    err=mcpwm_capture_timer_enable(timer);if(err!=ESP_OK)return err;
    return mcpwm_capture_timer_start(timer);
}
#endif
void pulse_capture_init(void){
    snapshot.pul_gpio=255;snapshot.dir_gpio=255;
#ifdef CONFIG_ROBOT_PULSE_CAPTURE_ENABLED
    esp_err_t err=setup();available=err==ESP_OK;
    if(!available)ESP_LOGE(TAG,"Capture unavailable: %s; motor/UART protocol unchanged",esp_err_to_name(err));
    else ESP_LOGI(TAG,"INPUT ONLY: PUL=GPIO%u DIR=GPIO%u, hardware clock=%lu Hz, window=100 ms",
        snapshot.pul_gpio,snapshot.dir_gpio,(unsigned long)snapshot.hz);
#else
    ESP_LOGI(TAG,"Pulse capture disabled");
#endif
}
void pulse_capture_poll(uint16_t conn,uint16_t handle,bool subscribed){
    int64_t now=esp_timer_get_time();
    if(!subscribed||conn==BLE_HS_CONN_HANDLE_NONE||conn!=previous_conn){
        portENTER_CRITICAL(&capture_lock);recording=false;portEXIT_CRITICAL(&capture_lock);
        state=0;next_us=now;previous_conn=conn;
        if(!subscribed||conn==BLE_HS_CONN_HANDLE_NONE)return;
    }
    if(state==0&&now>=next_us){
        snapshot.id++;snapshot.count=0;snapshot.flags=available?PULSE_AVAILABLE:0;
#ifdef CONFIG_ROBOT_PULSE_CAPTURE_ENABLED
        if(available){
            // The marker and both signal channels share one hardware counter.
            // No software timestamps are used for waveform edges.
            portENTER_CRITICAL(&capture_lock);
            esp_err_t err=mcpwm_capture_channel_trigger_soft_catch(channels[2]);
            if(err==ESP_OK)err=mcpwm_capture_get_latched_value(channels[2],&start_tick);
            snapshot.initial=(uint8_t)(gpio_get_level(snapshot.pul_gpio)|(gpio_get_level(snapshot.dir_gpio)<<1));
            recording=err==ESP_OK;portEXIT_CRITICAL(&capture_lock);
            if(err!=ESP_OK){snapshot.flags=0;available=false;}
        }
#endif
        started_us=now;state=1;
    }
    if(state==1&&(!available||now-started_us>=120000)){
        // Wait beyond the exact 100 ms hardware interval to drain pending ISRs.
        portENTER_CRITICAL(&capture_lock);recording=false;portEXIT_CRITICAL(&capture_lock);
        pulse_finalize(&snapshot);packet_index=0;transfer_us=now;state=2;
    }
    if(state==2){
        // Two small notifications per 20 ms tick; works with the default ATT MTU.
        // Status notifications and command processing retain their own path.
        for(unsigned n=0;n<2;n++){
            uint8_t data[20];if(!pulse_packet(&snapshot,packet_index,data))break;
            struct os_mbuf *om=ble_hs_mbuf_from_flat(data,sizeof(data));if(!om)break;
            int rc=ble_gatts_notify_custom(conn,handle,om);if(rc)break;
            if(++packet_index==pulse_packet_count(&snapshot)){state=0;next_us=started_us+1000000;break;}
        }
        // Never resume an old partial transfer indefinitely.
        if(state==2&&now-transfer_us>5000000){state=0;next_us=now+1000000;}
    }
}
