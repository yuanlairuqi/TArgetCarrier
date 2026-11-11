/**
 * @file    netcfg.h
 * @brief   靶车 —— 家里 WiFi 的账号配置 (两块 ESP32 共用, 存在各自的 NVS 里)
 *
 *  为什么要存而不是写死:
 *    密码不该进 Git 仓库, 换路由器也不用重新编译。两台板子(master / bridge)
 *    各自把自己那份 SSID/密码存本地 NVS, 上电自动连; 连不上就退回原本的行为
 *    (master 开热点 TargetCar 让你配网, bridge 维持信道 6)。
 *
 *  ★ ESP-NOW 的信道必须两端一致: 两块板都连同一个路由器之后, 信道会自动
 *    跟随路由器的 2.4G 信道, 所以比"写死 6"更省心。
 */

#ifndef NETCFG_H
#define NETCFG_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 读 WiFi 账号; 没存过返回 false */
bool netcfg_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

/** 存 WiFi 账号 (写入 NVS, 掉电不丢) */
bool netcfg_save(const char *ssid, const char *pass);

/** 清掉已存的账号 */
void netcfg_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* NETCFG_H */
