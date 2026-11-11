/**
 * @file    web_ui.cpp
 * @brief   靶车 —— 主机内置网页控制台 (HTTP + 极简前端)
 *
 *  接口:
 *    GET  /               内置 HTML 页面
 *    GET  /api/status     状态 JSON (由 main.cpp 的 app_status_json 生成, 最坏 <1000 B)
 *    GET  /api/cmd?c=xxx  执行一行控制台命令 (与串口同一套, 见 handle_command)
 *                         J/D/b 通过主循环安排撤销授权后的延迟重启，不阻塞控制循环。
 *
 *  ★ 命令不走状态机、不加新协议: 网页发的就是串口控制台的同一行文本,
 *    所以"网页能做什么"永远等于"串口能做什么", 不会两边行为不一致。
 */

#include "web_ui.h"
#include "app_config.h"
#include "netcfg.h"
#include "param_store.h"
#include "protocol.h"
#include "web_index.h"    /* Vue 版页面 (web/index.html) */
#include "web_vue.h"      /* Vue 3 运行时 (web/vue.global.prod.js) */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>

static WebServer s_server(80);

/* ------------------------------------------------------------------ */
/* 前端页面 (压缩过的单文件, 手机竖屏优先)                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* HTTP 处理                                                          */
/* ------------------------------------------------------------------ */

static void handle_root(void)
{
    /* ★ 必须显式禁止缓存: 之前烧了新固件, 手机/电脑浏览器还在用缓存里的旧页面,
     *   表现就是"我明明更新了, 打开还是原来的"。 */
    s_server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    s_server.sendHeader("Pragma", "no-cache");
    s_server.sendHeader("Expires", "0");
    s_server.send_P(200, "text/html; charset=utf-8", kIndexHtml);
}

/* Vue 3 运行时: 体积大 (170 KB), 让浏览器缓存一年 */
static void handle_vue_js(void)
{
    s_server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
    s_server.send_P(200, "application/javascript; charset=utf-8", kVueJs);
}

static void handle_status(void)
{
    /* ★ v1.1: 状态 JSON 增加安全/授权/发送统计字段 (契约 §7), 最坏接近 1000 B,
     *   缓冲从 768 提到 1024; 截断时必须返回错误而不是发半截 JSON。 */
    char buf[1024];
    size_t n = app_status_json(buf, sizeof(buf));
    if (n == 0) {
        s_server.send(500, "application/json", "{\"err\":1}");
        return;
    }
    s_server.send(200, "application/json", String(buf));
}

static void handle_cmd(void)
{
    String c = s_server.arg("c");
    c.trim();
    if (c.length() == 0 || c.length() > 40) {
        s_server.send(400, "application/json", "{\"ok\":0}");
        return;
    }
    /* J/D/b are supported again; main schedules a disabled, nonblocking reboot. */
    app_run_command(c.c_str());
    s_server.send(200, "application/json", "{\"ok\":1}");
}

/* ---------------- 配网页面 /wifi ---------------- */

static const char kWifiHtml[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>配网 · 靶车</title><style>
body{margin:0;background:#101317;color:#e8edf3;font:15px/1.5 -apple-system,"Segoe UI","Microsoft YaHei",sans-serif;padding:16px}
.card{background:#1b2027;border-radius:12px;padding:14px;max-width:420px;margin:0 auto}
h1{font-size:18px;margin:4px 0 12px}
label{display:block;color:#8b97a6;font-size:13px;margin:10px 0 4px}
input{width:100%;padding:10px;border-radius:9px;border:1px solid #333c48;background:#0d1116;color:#e8edf3;font:15px inherit}
button{margin-top:14px;width:100%;padding:12px;border:0;border-radius:10px;background:#3b82f6;color:#fff;font:600 16px inherit}
.hint{color:#8b97a6;font-size:12px;margin-top:10px;line-height:1.5}
a{color:#60a5fa}
</style></head><body><div class="card">
<h1>连上家里的 WiFi</h1>
<form method="get" action="/wifi">
  <label>WiFi 名称 (2.4G)</label>
  <input name="ssid" placeholder="例如 rm_2.4G" required>
  <label>密码</label>
  <input name="pass" type="password" placeholder="至少 8 位" required>
  <button type="submit">保存并重启</button>
</form>
<div class="hint">保存后主机会重启并尝试连接；成功后手机/电脑在同一个 WiFi 下直接打开
主机的 IP（串口会打印）即可，不用再切热点。<br>
两台板子（主机 + 桥接）要连同一个 WiFi，ESP-NOW 的信道会自动跟着路由器走。<br>
连不上会自动退回热点 <b>TargetCar</b>。</div>
</div></body></html>)HTML";

static void handle_wifi(void)
{
    String ssid = s_server.arg("ssid");
    String pass = s_server.arg("pass");
    ssid.trim();
    pass.trim();

    if (ssid.length() == 0 || pass.length() < 8) {
        s_server.send_P(200, "text/html; charset=utf-8", kWifiHtml);
        return;
    }
    if (!netcfg_save(ssid.c_str(), pass.c_str())) {
        s_server.send(500, "text/plain; charset=utf-8", "保存失败");
        return;
    }
    s_server.send(200, "text/html; charset=utf-8", String(
        "<meta charset='utf-8'><body style='background:#101317;color:#e8edf3;"
        "font-family:sans-serif;padding:24px'>已保存，3 秒后重启连接 <b>") + ssid +
        "</b> …<br><br>连上后请用主机串口打印的 IP 打开控制页面。</body>");
    delay(1500);
    ESP.restart();
}

/* ---------------- 电机参数 (CAN 0x7FF) ---------------- */

static void handle_param(void)
{
    String op = s_server.arg("op");
    int rid = s_server.arg("rid").toInt();
    if (rid < 0 || rid >= 64) {
        s_server.send(400, "application/json", "{\"ok\":0}");
        return;
    }

    uint8_t value[4] = {0, 0, 0, 0};
    bool is_float = s_server.arg("float") == "1";

    if (op == "read") {
        s_server.send(200, "application/json",
                      app_param_request(PROTO_PARAM_OP_READ, (uint8_t)rid, value)
                          ? "{\"ok\":1}" : "{\"ok\":0}");
        return;
    }
    if (op == "save") {
        s_server.send(200, "application/json",
                      app_param_request(PROTO_PARAM_OP_SAVE, 0, value)
                          ? "{\"ok\":1}" : "{\"ok\":0}");
        return;
    }
    if (op == "write") {
        String vs = s_server.arg("val");
        if (is_float) {
            float f = vs.toFloat();
            memcpy(value, &f, 4);
            s_server.send(200, "application/json",
                          app_param_request(PROTO_PARAM_OP_WRITE_F, (uint8_t)rid, value)
                              ? "{\"ok\":1}" : "{\"ok\":0}");
        } else {
            uint32_t u = (uint32_t)vs.toInt();
            value[0] = (uint8_t)(u & 0xFF);
            value[1] = (uint8_t)((u >> 8) & 0xFF);
            value[2] = (uint8_t)((u >> 16) & 0xFF);
            value[3] = (uint8_t)((u >> 24) & 0xFF);
            s_server.send(200, "application/json",
                          app_param_request(PROTO_PARAM_OP_WRITE_U, (uint8_t)rid, value)
                              ? "{\"ok\":1}" : "{\"ok\":0}");
        }
        return;
    }
    s_server.send(400, "application/json", "{\"ok\":0}");
}

static void handle_param_state(void)
{
    char buf[900];
    size_t n = app_param_values_json(buf, sizeof(buf));
    s_server.send(200, "application/json", n ? String(buf) : "{}");
}

/* ---------------- 参数预设 ---------------- */

static void handle_presets(void)
{
    /* 4 个槽位、名字最长 15 字符时, 完整 JSON 约 400 字节; 留足余量防止截断成非法 JSON */
    char buf[900];
    size_t n = app_presets_json(buf, sizeof(buf));
    if (n == 0) {
        s_server.send(500, "application/json", "[]");
        return;
    }
    s_server.send(200, "application/json", String(buf));
}

static bool preset_slot_from_arg(int *slot_out)
{
    int i = s_server.arg("i").toInt();
    if (i < 0 || i >= PRESET_SLOTS) {
        s_server.send(400, "application/json", "{\"ok\":0}");
        return false;
    }
    *slot_out = i;
    return true;
}

static void handle_preset_save(void)
{
    int slot;
    if (!preset_slot_from_arg(&slot)) {
        return;
    }
    String name = s_server.arg("name");
    name.trim();
    bool ok = app_preset_save((uint8_t)slot, name.c_str());
    s_server.send(200, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0}");
}

static void handle_preset_load(void)
{
    int slot;
    if (!preset_slot_from_arg(&slot)) {
        return;
    }
    bool ok = app_preset_load((uint8_t)slot);
    s_server.send(200, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0}");
}

static void handle_preset_del(void)
{
    int slot;
    if (!preset_slot_from_arg(&slot)) {
        return;
    }
    bool ok = app_preset_clear((uint8_t)slot);
    s_server.send(200, "application/json", ok ? "{\"ok\":1}" : "{\"ok\":0}");
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

void web_ui_begin(void)
{
    s_server.on("/", handle_root);
    s_server.on("/vue.js", handle_vue_js);
    s_server.on("/api/status", handle_status);
    s_server.on("/api/cmd", handle_cmd);
    s_server.on("/api/param", handle_param);
    s_server.on("/api/param/state", handle_param_state);
    s_server.on("/api/presets", handle_presets);
    s_server.on("/api/preset/save", handle_preset_save);
    s_server.on("/api/preset/load", handle_preset_load);
    s_server.on("/api/preset/del", handle_preset_del);
    s_server.on("/wifi", handle_wifi);
    s_server.onNotFound([]() {
        s_server.send(404, "text/plain", "not found");
    });
    s_server.begin();
    Serial.println(F("[WEB] HTTP 服务已启动 (:80), 配网页面 /wifi, 预设 /api/presets"));
}

void web_ui_poll(void)
{
    s_server.handleClient();
}
