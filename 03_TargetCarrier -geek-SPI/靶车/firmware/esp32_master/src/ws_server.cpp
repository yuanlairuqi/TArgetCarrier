/**
 * @file    ws_server.c(pp)
 * @brief   主机上的极简 WebSocket 推送服务 (RFC 6455 子集)
 *
 *  行为:
 *    - 监听 81 端口; 同一时刻只服务一个客户端 (够用且更省 RAM)
 *    - 握手成功后, 每 50 ms 推一帧状态 JSON (20 Hz, 给实时曲线用)
 *    - 收到客户端文本帧 -> 当作控制台命令执行 (和 HTTP /api/cmd 同一路径)
 *    - 收到 ping -> 回 pong; 收到 close / 断开 -> 清掉客户端
 *
 *  ★ 稳定性取舍:
 *    - 协议解析失败 = 直接断开, 不做半包状态机 (客户端是我们自己的页面);
 *    - 所有发送都检查返回值, 失败就断开, 不会卡住任务;
 *    - 任务栈 5 KB (v1.1 状态 JSON 新增安全/授权/统计字段, 最坏路径
 *      json[1024] + 发送帧[1024] 同栈), 低优先级, 放核心 0, 不干扰核心 1 的 50Hz 控制循环。
 */

#include "ws_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "ws_proto.h"
#include "web_ui.h"

#define WS_PORT            81
#define WS_PUSH_PERIOD_MS  50
#define WS_RX_BUF          512
/* ★ v1.1: 状态 JSON 增加 st_fresh/st_online/br_age/br_flags/safe/safe_reason/
 *   arm/arm_token/arm_rej/q_drop/cmd_age_drop/tx_* 共 17 个字段 (契约 §7),
 *   最坏长度接近 1000 B, 发送缓冲必须 >= JSON + 帧头, 所以从 512 提到 1024。 */
#define WS_TX_BUF          1024
/* v1.1: ws_task 栈从 4096 提到 5120 —— json[1024] + ws_send_text 的 frame[1024]
 *   在同一任务栈上叠加, 4096 余量不足 (契约 §4.6)。 */
#define WS_TASK_STACK      5120

static WiFiServer s_srv(WS_PORT);
static WiFiClient s_cli;
static TaskHandle_t s_task = nullptr;
static volatile int s_clients = 0;

/* ------------------------------------------------------------------ */
/* 握手                                                               */
/* ------------------------------------------------------------------ */

/** 从 HTTP 请求头里取 Sec-WebSocket-Key; 成功则回 101 并返回 true */
static bool ws_handshake(WiFiClient &c)
{
    char line[160];
    char key[64] = {0};
    uint32_t deadline = millis() + 1500;

    while (millis() < deadline) {
        if (!c.available()) {
            delay(2);
            continue;
        }
        int n = c.readBytesUntil('\n', line, sizeof(line) - 1);
        if (n <= 0) {
            continue;
        }
        line[n] = '\0';
        /* 去掉 \r */
        char *cr = strchr(line, '\r');
        if (cr) {
            *cr = '\0';
        }
        if (line[0] == '\0') {
            break;                 /* 空行 = 头部结束 */
        }
        /* 大小写不敏感地找 Sec-WebSocket-Key */
        if (strncasecmp(line, "Sec-WebSocket-Key:", 18) == 0) {
            const char *v = line + 18;
            while (*v == ' ') {
                v++;
            }
            strncpy(key, v, sizeof(key) - 1);
        }
    }

    if (key[0] == '\0') {
        c.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
        return false;
    }

    char accept[29];
    if (!ws_accept_key(key, accept)) {
        c.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
        return false;
    }

    c.printf("HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    return true;
}

/* ------------------------------------------------------------------ */
/* 帧收发                                                             */
/* ------------------------------------------------------------------ */

static bool ws_send_text(const char *payload, size_t len)
{
    uint8_t frame[WS_TX_BUF];
    size_t n = ws_encode_text(payload, len, frame, sizeof(frame));
    if (n == 0) {
        return false;
    }
    return s_cli.write(frame, n) == n;
}

/** 处理客户端发来的一帧; 返回 false 表示连接应断开 */
static bool ws_handle_frame(const uint8_t *buf, size_t total)
{
    uint8_t opcode;
    bool masked;
    size_t plen, hlen;
    if (ws_parse_header(buf, total, &opcode, &masked, &plen, &hlen) <= 0) {
        return false;
    }
    if (total < hlen + plen) {
        return false;
    }

    uint8_t payload[WS_RX_BUF];
    if (plen >= sizeof(payload)) {
        return false;
    }
    memcpy(payload, buf + hlen, plen);
    if (masked) {
        ws_apply_mask(payload, plen, buf + hlen - 4);
    }
    payload[plen] = '\0';

    switch (opcode) {
    case WS_OP_TEXT:
        /* ★ 本任务在核心 0, 不能直接改控制状态; 投递给主循环执行 */
        app_run_command_async((const char *)payload);
        return true;
    case WS_OP_PING: {
        /* 回 pong: 复用文本编码函数再改 opcode */
        uint8_t frame[WS_TX_BUF];
        size_t n = ws_encode_text((const char *)payload, plen, frame, sizeof(frame));
        if (n == 0) {
            return false;
        }
        frame[0] = (uint8_t)(0x80u | WS_OP_PONG);
        return s_cli.write(frame, n) == n;
    }
    case WS_OP_CLOSE:
        return false;
    default:
        return true;   /* 其它帧忽略, 不断连接 */
    }
}

/* ------------------------------------------------------------------ */
/* 任务                                                               */
/* ------------------------------------------------------------------ */

static void ws_task(void *arg)
{
    (void)arg;
    uint32_t last_push = 0;

    for (;;) {
        /* 1. 接新客户端 */
        WiFiClient incoming = s_srv.available();
        if (incoming) {
            if (s_cli && s_cli.connected()) {
                incoming.stop();          /* 只服务一个 */
            } else {
                s_cli = incoming;
                if (ws_handshake(s_cli)) {
                    /* 客户端写超时: 卡住的客户端最多拖 200 ms, 之后写失败就断开,
                     * 不会让 WS 任务永久阻塞 (控制循环在另一个核, 但也不能留隐患) */
                    s_cli.setTimeout(200);
                    s_cli.setNoDelay(true);
                    s_clients = 1;
                    last_push = 0;
                    Serial.println(F("[WS] 客户端已连接 (20Hz 推送)"));
                } else {
                    s_cli.stop();
                    s_clients = 0;
                }
            }
        }

        if (!s_cli || !s_cli.connected()) {
            if (s_clients) {
                s_clients = 0;
                Serial.println(F("[WS] 客户端断开"));
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 2. 收客户端帧 (控制命令) */
        if (s_cli.available()) {
            uint8_t buf[WS_RX_BUF];
            int n = s_cli.read(buf, sizeof(buf));
            if (n > 0) {
                if (!ws_handle_frame(buf, (size_t)n)) {
                    s_cli.stop();
                    s_clients = 0;
                    continue;
                }
            }
        }

        /* 3. 定时推状态 */
        uint32_t now = millis();
        if ((uint32_t)(now - last_push) >= WS_PUSH_PERIOD_MS) {
            last_push = now;
            /* ★ v1.1: 状态 JSON 带安全/授权/发送统计字段 (契约 §7), 最坏 <1000 B,
             *   所以缓冲从 768 提到 1024; 仍要检查返回值, 截断时宁可断开也不发半截 JSON。 */
            char json[1024];
            size_t len = app_status_json(json, sizeof(json));
            if (len == 0 || !ws_send_text(json, len)) {
                s_cli.stop();
                s_clients = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

void ws_server_begin(void)
{
    s_srv.begin();
    s_srv.setNoDelay(true);
    if (s_task == nullptr) {
        xTaskCreatePinnedToCore(ws_task, "ws", WS_TASK_STACK, nullptr, 3, &s_task, 0);
    }
    Serial.printf("[WS] WebSocket 推送已启动: ws://<IP>:%d/\n", WS_PORT);
}

int ws_server_clients(void)
{
    return s_clients;
}
