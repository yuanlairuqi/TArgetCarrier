#include "capture_core/source.h"
#include "link_led.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "os/os_mbuf.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "robot_control.h"
#include "battery.h"
#include "robot_bridge.h"
#include "uart_link.h"
#include "esp_random.h"
#include "pulse_capture.h"
#include "now_protocol.h"
#include "now_radio.h"
#include "control_feedback.h"
#include "diagnostic_wire.h"
#include "notify_diag.h"
#include "oled_status.h"
#include "esp_system.h"
#include "limit_inputs.h"
static now_control_t wireless;
static bool wireless_available;
static void put32w(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void put16w(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static cap_source recording_source;
static bool record_send(const uint8_t fragment[20],void *ctx){
    (void)ctx;uint8_t out[64]={'W','N',1,4};
    put32w(out+4,wireless.node);put32w(out+8,wireless.boot);put32w(out+12,wireless.epoch);
    put32w(out+16,wireless.session);out[32]=3;
    memcpy(out+40,fragment,20);put32w(out+60,now_crc32(out,60));return now_radio_send_wave(out);
}
static bool radio_wave(const uint8_t packet[20],uint32_t start,uint16_t index,uint16_t total){
    uint8_t out[64]={ 'W','N',1,4 };
    put32w(out+4,wireless.node);put32w(out+8,wireless.boot);put32w(out+12,wireless.epoch);
    put32w(out+16,wireless.session);put32w(out+24,start);
    out[28]=(uint8_t)index;out[29]=(uint8_t)(index>>8);out[30]=(uint8_t)total;out[31]=(uint8_t)(total>>8);
    out[32]=1; // source 1 = slave MCPWM GPIO capture, not motor encoder
    memcpy(out+40,packet,20);put32w(out+60,now_crc32(out,60));return now_radio_send_wave(out);
}
// UUIDs: 6e400001/2/3-b5a3-f393-e0a9-e50e24dcca9e, little endian in NimBLE.
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);
static const ble_uuid128_t command_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);
static const ble_uuid128_t status_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);
static const ble_uuid128_t pulse_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x04,0x00,0x40,0x6e);
static const ble_uuid128_t diagnostic_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x05,0x00,0x40,0x6e);
/* Diagnostics-only read-only counters. Read on demand by the PC; never notified,
 * so they add no traffic of their own. See notify_diag.h for field layout. */
static const ble_uuid128_t ext1_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x06,0x00,0x40,0x6e);
static const ble_uuid128_t ext2_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x07,0x00,0x40,0x6e);
static const ble_uuid128_t ext3_uuid = BLE_UUID128_INIT(
    0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x08,0x00,0x40,0x6e);
static uint16_t diagnostic_handle;
static bool diagnostic_subscribed;
static diagnostic_state_t diagnostic;
static uint32_t diagnostic_loop_max;
static uint8_t diagnostic_notify_error;
static uint16_t pulse_handle;
static bool pulse_subscribed;
/* Declared before the EXT read callbacks, which report the live connection. */
static uint16_t status_handle, connection = BLE_HS_CONN_HANDLE_NONE;
static int access_pulse(uint16_t c,uint16_t a,struct ble_gatt_access_ctxt *ctx,void *arg){
    (void)c;(void)a;(void)ctx;(void)arg;return BLE_ATT_ERR_READ_NOT_PERMITTED;
}
/* EXT1 (0xE1, layout 1): notify outcome counters, 20 B little endian.
 * [0]=0xE1 [1]=0x01 [2..3] status_attempt [4..5] status_fail [6..7] status_enomem
 * [8..9] status_mbuf_null [10..11] pulse_ok [12..13] pulse_fail [14..15] pulse_enomem
 * [16] pulse_mbuf_null (u8 sat) [17] last_fail_kind [18] last_fail_rc
 * [19] last_fail_age_ds (0.1 s units, 255 = never). */
static int access_ext1(uint16_t c,uint16_t a,struct ble_gatt_access_ctxt *ctx,void *arg){
    (void)c;(void)a;(void)arg;uint8_t bytes[20]={0};
    bytes[0]=0xE1;bytes[1]=0x01;
    put16w(bytes+2,notify_diag.status_attempt);put16w(bytes+4,notify_diag.status_fail);
    put16w(bytes+6,notify_diag.status_enomem);put16w(bytes+8,notify_diag.status_mbuf_null);
    put16w(bytes+10,notify_diag.pulse_ok);put16w(bytes+12,notify_diag.pulse_fail);
    put16w(bytes+14,notify_diag.pulse_enomem);
    bytes[16]=notify_diag.pulse_mbuf_null;
    bytes[17]=notify_diag.last_fail_kind;bytes[18]=notify_diag.last_fail_rc;
    uint32_t age=255U;
    if(notify_diag.last_fail_kind){
        age=(uint32_t)(notify_diag_now_ms()-notify_diag.last_fail_ms)/100U;
        if(age>255U)age=255U;
    }
    bytes[19]=(uint8_t)age;
    return os_mbuf_append(ctx->om,bytes,20)==0?0:BLE_ATT_ERR_INSUFFICIENT_RES;
}
/* EXT2 (0xE2, layout 1): command path / UART / lock, 20 B little endian.
 * [0]=0xE2 [1]=0x01 [2..5] ble_cmd_count [6..9] ble_cmd_last_seq
 * [10..11] ble_cmd_last_age_ms (0xFFFF = never) [12..13] uart_tx_ok
 * [14..15] uart_tx_fail [16..17] att_lock_wait_max_ms [18] att_lock_wait_last_ms
 * [19] telemetry_gap_max_ms (sat 255). */
static int access_ext2(uint16_t c,uint16_t a,struct ble_gatt_access_ctxt *ctx,void *arg){
    (void)c;(void)a;(void)arg;uint8_t bytes[20]={0};
    bytes[0]=0xE2;bytes[1]=0x01;
    put32w(bytes+2,notify_diag.ble_cmd_count);put32w(bytes+6,notify_diag.ble_cmd_last_seq);
    uint32_t age=(uint32_t)(notify_diag_now_ms()-notify_diag.ble_cmd_last_ms);
    put16w(bytes+10,(notify_diag.ble_cmd_count==0||age>0xFFFFU)?0xFFFFU:(uint16_t)age);
    put16w(bytes+12,notify_diag.uart_tx_ok);put16w(bytes+14,notify_diag.uart_tx_fail);
    put16w(bytes+16,notify_diag.att_lock_wait_max_ms);
    bytes[18]=notify_diag.att_lock_wait_last_ms;
    bytes[19]=notify_diag.telemetry_gap_max_ms;
    return os_mbuf_append(ctx->om,bytes,20)==0?0:BLE_ATT_ERR_INSUFFICIENT_RES;
}
/* EXT3 (0xE3, layout 1): link/pool/UART/diag counters, 20 B little endian.
 * [0]=0xE3 [1]=0x01 [2] conn_itvl_ms [3] conn_latency [4..5] att_mtu
 * [6..7] msys_free_min [8..9] msys_free_now [10..11] msys_total
 * [12..13] uart_rx_events [14..15] diag_ok [16..17] diag_fail
 * [18..19] pulse_transfer_abort_count. */
static int access_ext3(uint16_t c,uint16_t a,struct ble_gatt_access_ctxt *ctx,void *arg){
    (void)c;(void)a;(void)arg;uint8_t bytes[20]={0};
    bytes[0]=0xE3;bytes[1]=0x01;
    bytes[2]=notify_diag.conn_itvl_ms;bytes[3]=notify_diag.conn_latency;
    uint16_t mtu=connection!=BLE_HS_CONN_HANDLE_NONE?ble_att_mtu(connection):0;
    put16w(bytes+4,mtu);
    put16w(bytes+6,notify_diag.msys_free_min);put16w(bytes+8,notify_diag.msys_free_now);
    put16w(bytes+10,notify_diag.msys_total);
    put16w(bytes+12,notify_diag.uart_rx_events);
    put16w(bytes+14,notify_diag.diag_ok);put16w(bytes+16,notify_diag.diag_fail);
    put16w(bytes+18,notify_diag.pulse_transfer_abort_count);
    return os_mbuf_append(ctx->om,bytes,20)==0?0:BLE_ATT_ERR_INSUFFICIENT_RES;
}
static robot_control_t robot;
static SemaphoreHandle_t lock;
static uint8_t own_addr_type;
static bool subscribed;
static robot_bridge_t bridge;
static bool uart_available;
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }
static void diagnostic_update_locked(uint32_t now){
    diagnostic_update(&diagnostic,&robot,&bridge,now,diagnostic_loop_max,diagnostic_notify_error,(uint8_t)esp_reset_reason());
}
static int access_diagnostic(uint16_t c,uint16_t a,struct ble_gatt_access_ctxt *ctx,void *arg){
    (void)c;(void)a;(void)arg;uint8_t bytes[20];
    xSemaphoreTake(lock,portMAX_DELAY);diagnostic_update_locked(now_ms());memcpy(bytes,diagnostic.bytes,20);xSemaphoreGive(lock);
    return os_mbuf_append(ctx->om,bytes,20)==0?0:BLE_ATT_ERR_INSUFFICIENT_RES;
}
/* All callers hold lock; ISR only latches and wakes this high-priority task. */
static void apply_limit(void) {
    limit_inputs_sample(now_ms());
    if(limit_inputs_blocked())robot_latch_limit(&robot);
}
static bool reset_limit(void *ctx) {
    const robot_bridge_t *b = ctx;
    uint32_t now = now_ms();
    /* A stopped ESP request is insufficient: demand current, fresh, applied
     * STM32 STOP feedback before clearing the physical input latch. */
    control_feedback(&robot,b);
    if (!control_limit_reset_ready(&robot,b,now))
        return false;
    return limit_inputs_try_reset(now);
}
static void limit_stop_task(void *arg) {
    (void)arg;
    /* app_main publishes initialization outcome before enabling this worker. */
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    for(;;){
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5)>0?pdMS_TO_TICKS(5):1);
        xSemaphoreTake(lock,portMAX_DELAY);
        apply_limit();
        if(robot.limit_latched){
            uint32_t now=now_ms();wire_frame_t outgoing;
            bridge_profile(&bridge,robot.protocol,robot.accel,robot.decel);
            bridge_parameters(&bridge,0,0,0);
            /* opcode 0 is immediate STOP; opcode 3 is a ramped soft stop. */
            bridge_request(&bridge,robot.revision,ROBOT_STOP,0,robot.ack,now);
            if(bridge_tick(&bridge,now,&outgoing)&&!uart_link_send(&outgoing))bridge_tx_failed(&bridge);
        }
        xSemaphoreGive(lock);
    }
}
static int access_status(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg) {
    (void)conn; (void)attr; (void)arg;
    uint8_t data[ROBOT_STATUS_SIZE];
    xSemaphoreTake(lock, portMAX_DELAY); control_feedback(&robot,&bridge); robot_status(&robot, now_ms(), data); xSemaphoreGive(lock);
    return os_mbuf_append(ctx->om, data, sizeof(data)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
static int access_command(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg) {
    (void)conn; (void)attr; (void)arg;
    /* Diagnostics: count every ATT write that actually reaches this callback and
     * how long the robot mutex kept us waiting. Measurement only. */
    uint64_t entered_us=esp_timer_get_time();
    uint8_t data[ROBOT_CMD_SIZE];
    uint16_t len = 0;
    if (OS_MBUF_PKTLEN(ctx->om) != sizeof(data) ||
        ble_hs_mbuf_to_flat(ctx->om, data, sizeof(data), &len) != 0) {
        xSemaphoreTake(lock, portMAX_DELAY);
        uint32_t wait_ms=(uint32_t)((esp_timer_get_time()-entered_us)/1000);
        if(wait_ms>notify_diag.att_lock_wait_max_ms)notify_diag.att_lock_wait_max_ms=(uint16_t)(wait_ms>0xFFFFU?0xFFFFU:wait_ms);
        notify_diag.att_lock_wait_last_ms=(uint8_t)(wait_ms>255U?255U:wait_ms);
        robot_stop(&robot,ROBOT_BAD_COMMAND);
        xSemaphoreGive(lock);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    /* robot_control.c reads the phone sequence from bytes 4..7 little endian.
     * Only writes with a valid 16 B payload are counted (bad length excluded). */
    notify_diag.ble_cmd_count++;
    notify_diag.ble_cmd_last_seq=(uint32_t)data[4]|((uint32_t)data[5]<<8)|
        ((uint32_t)data[6]<<16)|((uint32_t)data[7]<<24);
    notify_diag.ble_cmd_last_ms=now_ms();
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t wait_ms=(uint32_t)((esp_timer_get_time()-entered_us)/1000);
    if(wait_ms>notify_diag.att_lock_wait_max_ms)notify_diag.att_lock_wait_max_ms=(uint16_t)(wait_ms>0xFFFFU?0xFFFFU:wait_ms);
    notify_diag.att_lock_wait_last_ms=(uint8_t)(wait_ms>255U?255U:wait_ms);
    apply_limit();
    bool accepted=now_ble_command(&wireless,&robot,data,len,now_ms());
    apply_limit(); /* A racing input edge immediately relatches after reset. */
    diagnostic_update_locked(now_ms());
    uint8_t motion=robot.motion, speed=robot.speed;
    xSemaphoreGive(lock);
    ESP_LOGI("robot", "BLE command %s: motion=%u speed=%u; STM32 acknowledgement is separate",
        accepted ? "accepted" : "rejected", motion, speed);
    /* ATT confirms receipt only. Application rejection/replay is reported in
     * status/ACK and must not tear down BLE; robot_command still stops/rejects
     * invalid or unsafe commands. Wrong attribute length is rejected above. */
    return 0;
}
static const struct ble_gatt_svc_def services[] = {
    {.type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=&service_uuid.u,
     .characteristics=(struct ble_gatt_chr_def[]) {
        {.uuid=&command_uuid.u, .access_cb=access_command, .flags=BLE_GATT_CHR_F_WRITE},
        {.uuid=&status_uuid.u, .access_cb=access_status,
         .flags=BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle=&status_handle},
        {.uuid=&diagnostic_uuid.u, .access_cb=access_diagnostic,
         .flags=BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle=&diagnostic_handle},
        {.uuid=&pulse_uuid.u, .access_cb=access_pulse,
         .flags=BLE_GATT_CHR_F_NOTIFY, .val_handle=&pulse_handle},
        {.uuid=&ext1_uuid.u, .access_cb=access_ext1, .flags=BLE_GATT_CHR_F_READ},
        {.uuid=&ext2_uuid.u, .access_cb=access_ext2, .flags=BLE_GATT_CHR_F_READ},
        {.uuid=&ext3_uuid.u, .access_cb=access_ext3, .flags=BLE_GATT_CHR_F_READ},
        {0}}}, {0}
};
static int gap_event(struct ble_gap_event *event, void *arg);
static void advertise(void) {
    struct ble_hs_adv_fields fields = {0}, response = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&service_uuid;
    fields.num_uuids128 = 1; fields.uuids128_is_complete = 1;
    const char *name = ble_svc_gap_device_name();
    response.name = (uint8_t *)name; response.name_len = strlen(name); response.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&response);
    struct ble_gap_adv_params params = {.conn_mode=BLE_GAP_CONN_MODE_UND, .disc_mode=BLE_GAP_DISC_MODE_GEN};
    if (rc == 0) rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc) ESP_LOGE("robot", "Advertising failed: %d", rc);
    else ESP_LOGI("robot", "Advertising TC-XAxis");
}
/* Diagnostics only: remember the negotiated link parameters for EXT3.
 * Recording only; the connection itself is not touched. */
static void conn_params_snapshot(uint16_t handle){
    struct ble_gap_conn_desc desc;
    if(ble_gap_conn_find(handle,&desc)!=0)return;
    uint32_t itvl_ms=((uint32_t)desc.conn_itvl*125U+500U)/1000U; /* 1.25 ms units */
    notify_diag.conn_itvl_ms=(uint8_t)(itvl_ms>255U?255U:itvl_ms);
    notify_diag.conn_latency=(uint8_t)(desc.conn_latency>255U?255U:desc.conn_latency);
}
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            xSemaphoreTake(lock, portMAX_DELAY);
            connection=event->connect.conn_handle; subscribed=false; pulse_subscribed=false; diagnostic_subscribed=false;
            now_ble_connection(&wireless, &robot, true); xSemaphoreGive(lock);
            conn_params_snapshot(event->connect.conn_handle);
            ESP_LOGI("robot", "Phone connected");
        } else advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        xSemaphoreTake(lock, portMAX_DELAY);
        connection=BLE_HS_CONN_HANDLE_NONE; subscribed=false; pulse_subscribed=false; diagnostic_subscribed=false;
        now_ble_connection(&wireless, &robot, false); xSemaphoreGive(lock);
        notify_diag.conn_itvl_ms=0;notify_diag.conn_latency=0;
        ESP_LOGI("robot", "Disconnected reason=%d: control stopped",event->disconnect.reason); advertise(); break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        /* Diagnostics only: refreshed by the central's connection parameters. */
        if(event->conn_update.status==0)conn_params_snapshot(event->conn_update.conn_handle);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if(event->subscribe.attr_handle==diagnostic_handle){
            xSemaphoreTake(lock,portMAX_DELAY);diagnostic_subscribed=event->subscribe.cur_notify;xSemaphoreGive(lock);
        }
        if (event->subscribe.attr_handle == status_handle) {
            xSemaphoreTake(lock, portMAX_DELAY);
            subscribed = event->subscribe.cur_notify; xSemaphoreGive(lock);
        }
        if (event->subscribe.attr_handle == pulse_handle) {
            xSemaphoreTake(lock, portMAX_DELAY);
            pulse_subscribed=event->subscribe.cur_notify; xSemaphoreGive(lock);
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE: advertise(); break;
    default: break;
    }
    return 0;
}
static void sync_host(void) {
    int rc=ble_hs_util_ensure_addr(0); assert(rc == 0);
    rc=ble_hs_id_infer_auto(0, &own_addr_type); assert(rc == 0);
    advertise();
}
static void reset_host(int reason) {
    xSemaphoreTake(lock, portMAX_DELAY);
    subscribed=false; pulse_subscribed=false; diagnostic_subscribed=false; connection=BLE_HS_CONN_HANDLE_NONE; now_ble_connection(&wireless, &robot, false);
    xSemaphoreGive(lock);
    ESP_LOGE("robot", "BLE host reset: %d", reason);
}
static void host_task(void *arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }
static void telemetry_task(void *arg) {
    (void)arg; uint32_t last_battery=0,last_notify=0,last_diagnostic=0;uint16_t esp_mv=0;bool esp_battery_valid=false;
    wire_parser_t parser={0};uint32_t last_radio_status=0,last_loop=0,max_gap=0,radio_generation=0,diag_at=0,resource_at=0;
    for (;;) {
        uint32_t now=now_ms();
        if(last_loop && now-last_loop>max_gap)max_gap=now-last_loop;
        last_loop=now;
        /* Diagnostics only: longest telemetry interval since boot (u8 saturated
         * in EXT2) and since the last 5 s report (log only). */
        uint8_t gap_u8=(uint8_t)(max_gap>255U?255U:max_gap);
        if(gap_u8>notify_diag.telemetry_gap_max_ms)notify_diag.telemetry_gap_max_ms=gap_u8;
        if(max_gap>notify_diag.telemetry_gap_window_max_ms)notify_diag.telemetry_gap_window_max_ms=(uint16_t)(max_gap>0xFFFFU?0xFFFFU:max_gap);
        now_radio_stats_t rs;now_radio_stats(&rs);
        uint16_t mv=0; bool sample=(uint32_t)(now-last_battery)>=500;
        bool valid=sample ? battery_read(&mv) : false;
        uint8_t data[ROBOT_STATUS_SIZE],diagnostic_data[20]; uint16_t conn; bool notify,pulse_notify,diagnostic_notify;
        uint8_t incoming[256];int count=uart_link_read(incoming,sizeof(incoming));wire_frame_t frame;
        xSemaphoreTake(lock,portMAX_DELAY);
        /* A BLE callback may accept a newer lease while we wait for this lock.
         * Refresh under the same lock before doing unsigned age arithmetic. */
        now=now_ms();
        apply_limit();
        for(int i=0;i<count;++i)if(wire_feed(&parser,incoming[i],&frame))bridge_receive(&bridge,&frame,now);
        bool transmit=false;wire_frame_t outgoing;
        if(radio_generation!=rs.recoveries){radio_generation=rs.recoveries;now_control_link_lost(&wireless,&robot);}
        control_feedback(&robot,&bridge);
        robot.link_error=robot.uart_configured?(uart_available?bridge.error:ROBOT_UART_TX_ERROR):ROBOT_OK;
        robot_tick(&robot, now);
        now_control_tick(&wireless,&robot,now);
        if(sample) { esp_battery_valid=valid;esp_mv=mv;last_battery=now; }
        robot.battery_valid=esp_battery_valid;robot.battery_mv=esp_mv;
        if(bridge.online&&(bridge.peer_flags&PEER_BATTERY)){robot.battery_valid=true;robot.battery_mv=bridge.battery_mv;}
        // Both inputs use one owner and one bridge; callbacks never touch the UART.
        // Poll only after applying STM32 state and lease expiry to reject obsolete commands.
        for(int batch=0;batch<4;++batch) {
            now_request_t request;uint32_t received;
            if(!now_radio_receive(&request,&received))break;
            if(request.node!=wireless.node)continue;
            apply_limit();
            uint8_t result=request.kind==NOW_QUERY?NOW_OK:
                now_control_command(&wireless,&robot,&request,received,now);
            uint8_t response[NOW_FRAME_SIZE];
            control_feedback(&robot,&bridge);
            now_status_frame(&wireless,&robot,&request,result,now,response);
            now_radio_send(response);
        }
        // Ping/retry cannot renew a motion lease.
        apply_limit();
        bridge_profile(&bridge,robot.protocol,robot.accel,robot.decel);
        bridge_parameters(&bridge,robot.mode,robot.forward_ms,robot.reverse_ms);
        bridge_request(&bridge,robot.revision,robot.motion,robot.speed,robot.ack,now);
        transmit=bridge_tick(&bridge,now,&outgoing);
        control_feedback(&robot,&bridge);
        robot.link_error=robot.uart_configured?(uart_available?bridge.error:ROBOT_UART_TX_ERROR):ROBOT_OK;
        robot_tick(&robot,now);
        apply_limit();
        // A timeout-generated stop supersedes any movement selected earlier in this tick.
        if(robot.motion==ROBOT_STOP&&transmit&&(outgoing.type==WIRE_COMMAND||outgoing.type==WIRE_PROFILE)&&outgoing.payload[0]!=0){
            bridge_request(&bridge,robot.revision,0,0,robot.ack,now);transmit=bridge_tick(&bridge,now,&outgoing);
        }
        /* Serialize UART writes with limit_stop_task. No stale motion frame may
         * be sent after a newer STOP by a task that released lock earlier. */
        if(transmit&&!uart_link_send(&outgoing))bridge_tx_failed(&bridge);
        diagnostic_loop_max=max_gap;diagnostic_update_locked(now);memcpy(diagnostic_data,diagnostic.bytes,20);
        robot_status(&robot, now, data); conn=connection; notify=subscribed; pulse_notify=pulse_subscribed;diagnostic_notify=diagnostic_subscribed;
        oled_status_t display={.current_hz=robot.current_hz,.target_hz=robot.target_hz,.stage=robot.stm_phase,.uptime=now/1000U,.motion=robot.motion,.speed=robot.speed,
            .error=robot.error?robot.error:robot.link_error,.ble=wireless.ble_connected,
            .stm_online=robot.stm_online,.stm_motion=robot.stm_motion,.stm_error=robot.stm_error,
            .confirmed=bridge.confirmed && !bridge.waiting && bridge.revision==robot.revision &&
                bridge.completed_phone==robot.ack && bridge.online && (robot.stm_flags&PEER_APPLIED),
            .source=wireless.active?2U:wireless.ble_connected?1U:0U};
        link_led_snapshot_t led={.ble=wireless.ble_connected,.geek=wireless.active,
            .have_rx=robot.has_seq,.rx_packets=robot.rx_count,.last_rx_ms=robot.last_motion_ms,
            .tx_ok=rs.link_ok,.tx_fail=rs.link_fail,
            .errors=rs.callback_timeout+rs.rx_drop+rs.control_drop+rs.expired+rs.auth_fail};
        static uint32_t record_at,last_error=UINT32_MAX;
        uint64_t record_us=(uint64_t)esp_timer_get_time();
        if((uint32_t)(now-record_at)>=CAP_SNAPSHOT_PERIOD_MS){record_at=now;
            if(robot.stm_online)cap_source_feed(&recording_source,CAP_SAMPLE,1,CAP_U32,1,record_us,robot.current_hz);
            if(esp_battery_valid)cap_source_feed(&recording_source,CAP_SAMPLE,2,CAP_U32,2,record_us,esp_mv);
            uint32_t code=(uint32_t)robot.stm_error|((uint32_t)robot.error<<8)|((uint32_t)robot.link_error<<16);
            if(code!=last_error){last_error=code;cap_source_feed(&recording_source,CAP_EVENT,100,CAP_U32,0,record_us,code);}
        }
        xSemaphoreGive(lock);
        link_led_publish(&led);
        if(notify && conn!=BLE_HS_CONN_HANDLE_NONE&&(uint32_t)(now-last_notify)>=100) {
            last_notify=now;
            notify_diag_status_attempt();
            struct os_mbuf *om=ble_hs_mbuf_from_flat(data, sizeof(data));
            if(!om)notify_diag_status_mbuf_null();
            if(om) { int rc=ble_gatts_notify_custom(conn, status_handle, om);
                if(rc)notify_diag_status_fail(rc);else notify_diag_status_ok();
                if(rc)diagnostic_notify_error=(uint8_t)rc;
                if(rc && rc!=BLE_HS_ENOTCONN) ESP_LOGD("robot", "Notify rc=%d", rc); }
        }
        /* Optional 20-byte historical fault snapshot; never queues motor commands. */
        if(diagnostic_notify && conn!=BLE_HS_CONN_HANDLE_NONE && (uint32_t)(now-last_diagnostic)>=1000){
            last_diagnostic=now;struct os_mbuf *om=ble_hs_mbuf_from_flat(diagnostic_data,20);
            if(om){int rc=ble_gatts_notify_custom(conn,diagnostic_handle,om);
                if(rc)notify_diag_diag_fail(rc);else notify_diag_diag_ok();}
            else notify_diag_diag_fail(BLE_HS_ENOMEM);
        }
        if(wireless_available&&(uint32_t)(now-last_radio_status)>=200) {
            uint8_t response[NOW_FRAME_SIZE];
            xSemaphoreTake(lock,portMAX_DELAY);
            now=now_ms(); /* Callbacks may have renewed a lease since unlock. */
            now_control_tick(&wireless,&robot,now);
            control_feedback(&robot,&bridge);
            now_status_frame(&wireless,&robot,NULL,NOW_OK,now,response);
            response[27]=1;response[28]=(uint8_t)esp_reset_reason();response[29]=(uint8_t)rs.rx_high;response[30]=(uint8_t)rs.tx_high;
            response[31]=(uint8_t)rs.wave_drop;response[32]=(uint8_t)rs.rx_drop;response[33]=(uint8_t)(rs.rx_drop>>8);
            response[34]=(uint8_t)rs.callback_timeout;response[35]=(uint8_t)(rs.callback_timeout>>8);
            response[36]=(uint8_t)rs.recoveries;response[37]=(uint8_t)(rs.recoveries>>8);
            response[38]=(uint8_t)max_gap;response[39]=(uint8_t)(max_gap>>8);
            put32w(response+60,now_crc32(response,60));
            xSemaphoreGive(lock);now_radio_send(response);last_radio_status=now;
        }
        xSemaphoreTake(lock,portMAX_DELAY);
        bool radio_capture=wireless_available&&wireless.active;
        xSemaphoreGive(lock);
        cap_source_poll(&recording_source,radio_capture,(uint64_t)esp_timer_get_time(),record_send,NULL);
        pulse_capture_sink(radio_capture?radio_wave:NULL);
        pulse_capture_poll(radio_capture?0xfffe:conn,pulse_handle,radio_capture||pulse_notify);
        now_radio_service();
        pulse_capture_status_t captured;pulse_capture_status(&captured);
        display.capture_available=captured.available;display.pulse_recent=captured.recent;
        display.pulse_hz=captured.hz;display.direction=captured.direction;
        oled_status_publish(&display);
        /* Diagnostics only: 1 Hz msys headroom sample (no allocation, no lock). */
        if((uint32_t)(now-resource_at)>=1000){resource_at=now;notify_diag_resource_sample();}
        if(now-diag_at>=5000){diag_at=now;
            ESP_LOGI("link_diag","rx_drop=%lu wave_drop=%lu cb_timeout=%lu generation=%lu loop_max_ms=%lu uart_error=%u auth_fail=%lu",
                (unsigned long)rs.rx_drop,(unsigned long)rs.wave_drop,(unsigned long)rs.callback_timeout,(unsigned long)rs.recoveries,(unsigned long)max_gap,display.error,(unsigned long)rs.auth_fail);
            /* Notify/pool/command counters. One bounded line per 5 s, no new BLE traffic. */
            ESP_LOGI("diag_cnt","st a/o/f/en/mn=%u/%u/%u/%u/%u pu o/f/e/mn=%u/%u/%u/%u dg o/f=%u/%u last kind/rc/age_ds=%u/%u/%lu msys_free=%d min=%u/%u uart tx_ok/fail/rx=%u/%u/%u cmd n/seq/age=%lu/%lu/%lu att_wait_max/last=%u/%u loop_max_win=%u conn=%u/%u mtu=%u abort=%u",
                (unsigned)notify_diag.status_attempt,(unsigned)notify_diag.status_ok,(unsigned)notify_diag.status_fail,
                (unsigned)notify_diag.status_enomem,(unsigned)notify_diag.status_mbuf_null,
                (unsigned)notify_diag.pulse_ok,(unsigned)notify_diag.pulse_fail,(unsigned)notify_diag.pulse_enomem,
                (unsigned)notify_diag.pulse_mbuf_null,
                (unsigned)notify_diag.diag_ok,(unsigned)notify_diag.diag_fail,
                (unsigned)notify_diag.last_fail_kind,(unsigned)notify_diag.last_fail_rc,
                (unsigned long)(notify_diag.last_fail_kind?(uint32_t)(now-notify_diag.last_fail_ms):0U),
                os_msys_num_free(),(unsigned)notify_diag.msys_free_min,(unsigned)notify_diag.msys_total,
                (unsigned)notify_diag.uart_tx_ok,(unsigned)notify_diag.uart_tx_fail,(unsigned)notify_diag.uart_rx_events,
                (unsigned long)notify_diag.ble_cmd_count,(unsigned long)notify_diag.ble_cmd_last_seq,
                (unsigned long)(notify_diag.ble_cmd_last_ms?(uint32_t)(now-notify_diag.ble_cmd_last_ms):0U),
                (unsigned)notify_diag.att_lock_wait_max_ms,(unsigned)notify_diag.att_lock_wait_last_ms,
                (unsigned)notify_diag.telemetry_gap_window_max_ms,
                (unsigned)notify_diag.conn_itvl_ms,(unsigned)notify_diag.conn_latency,
                (unsigned)(connection!=BLE_HS_CONN_HANDLE_NONE?ble_att_mtu(connection):0),
                (unsigned)notify_diag.pulse_transfer_abort_count);
            notify_diag.telemetry_gap_window_max_ms=0;
        }
        vTaskDelay(pdMS_TO_TICKS(10)>0?pdMS_TO_TICKS(10):1);
    }
}
void app_main(void) {
    link_led_init();
    esp_err_t ret=nvs_flash_init();
    if(ret==ESP_ERR_NVS_NO_FREE_PAGES || ret==ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ret=nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    lock=xSemaphoreCreateMutex(); assert(lock); robot_init(&robot); battery_init();
    uart_available=uart_link_init();
#ifdef CONFIG_ROBOT_UART_ENABLED
    robot.uart_configured=true;
#endif
    bridge_init(&bridge,uart_available,esp_random());
    robot.reset_limit=reset_limit;robot.reset_limit_ctx=&bridge;
    TaskHandle_t limit_task=NULL;
    if(xTaskCreate(limit_stop_task,"limit_stop",3072,NULL,12,&limit_task)!=pdPASS)abort();
    bool limit_ready=limit_inputs_init(limit_task);
    xSemaphoreTake(lock,portMAX_DELAY);
    apply_limit();
    xSemaphoreGive(lock);
    if(!limit_ready)ESP_LOGE("robot","Limit setup failed: all movement blocked");
    xTaskNotifyGive(limit_task);
#ifdef CONFIG_ROBOT_ESPNOW_ENABLED
    now_control_init(&wireless,CONFIG_ROBOT_ESPNOW_NODE_ID,esp_random());
#else
    now_control_init(&wireless,1,esp_random());
#endif
    cap_source_init(&recording_source,wireless.node,wireless.boot);
    wireless_available=now_radio_init();
    pulse_capture_init();
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb=sync_host; ble_hs_cfg.reset_cb=reset_host;
    ble_svc_gap_init(); ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(services)); ESP_ERROR_CHECK(ble_gatts_add_svcs(services));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set("TC-XAxis"));
    nimble_port_freertos_init(host_task);
    if(xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, NULL)!=pdPASS) abort();
    oled_status_start();
    ESP_LOGI("robot", "Ready: BLE v3 / optional encrypted ESP-NOW WN/1 / UART v2; motor control requires STM32 acknowledgement");
}
