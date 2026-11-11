/**
 * @file    web_ui.h
 * @brief   靶车 —— 主机内置网页控制台 (手机/电脑浏览器)
 *
 *  设计: 手机或电脑连上主机开出的 WiFi 热点, 浏览器打开 http://192.168.4.1
 *        网页通过 /api/cmd 发命令, 通过 /api/status 读状态。
 *
 *  ★ 为什么放在主机(master)上, 而不是桥接/STM32:
 *    - 只有主机在"决策"(轨迹/增益/使能位), 桥接和 STM32 只转发和执行;
 *    - 主机那块板本来就要插 USB 供电, 多开一个热点不增加硬件;
 *    - 网页和串口控制台共用同一套命令解析 (handle_command), 两边行为完全一致。
 *
 *  ★ 热点与 ESP-NOW 共存: 用 WIFI_AP_STA, 且热点信道必须 = ESPNOW_CHANNEL,
 *    否则射频切信道会直接断掉 ESP-NOW 链路。
 */

#ifndef WEB_UI_H
#define WEB_UI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 由 main.cpp 实现: 把当前状态拼成一行 JSON, 返回写入长度。
 * 形如 {"mode":"STATIC","yaw":0.00,...,"en":0,"estop":0,"ok":123,"fail":1}
 */
size_t app_status_json(char *buf, size_t n);

/**
 * 由 main.cpp 实现: 执行一行控制台命令 (与串口敲的是同一套, 例如 "1"、"k 40"、"E")。
 * ★ 只能在主循环线程 (loop) 里调 —— HTTP 处理就在 loop 里。
 */
void app_run_command(const char *line);

/**
 * 把命令"投递"给主循环执行 (线程安全)。
 * WebSocket 任务跑在核心 0, 不能直接改控制状态, 必须走这个队列。
 */
void app_run_command_async(const char *line);

/* ------------------------------------------------------------------ */
/* 参数预设 (main.cpp 里实现, 存 NVS)                                  */
/* ------------------------------------------------------------------ */

/** 列出所有预设的名字/参数, 返回 JSON 长度 (形如 [{"i":0,"name":"快","kp":..},...]) */
size_t app_presets_json(char *buf, size_t n);

/* ---- 电机参数读写 (转发到 DM 的 CAN 0x7FF 协议) ---- */
bool app_param_request(uint8_t op, uint8_t rid, const uint8_t value[4]);
size_t app_param_values_json(char *buf, size_t n);

/** 把当前参数存进第 slot 槽 */
bool app_preset_save(uint8_t slot, const char *name);

/** 把第 slot 槽的参数应用到当前 (并持久化) */
bool app_preset_load(uint8_t slot);

/** 清空第 slot 槽 */
bool app_preset_clear(uint8_t slot);

/** 启动热点 + HTTP 服务 (要在 ESP-NOW 初始化之后调用) */
void web_ui_begin(void);

/** 放进 loop(), 非阻塞地处理网页请求 */
void web_ui_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_UI_H */
