/**
 * @file    ws_server.h
 * @brief   靶车 —— 主机上的极简 WebSocket 推送服务 (端口 81)
 *
 *  ★ 为什么自己写而不是引第三方库:
 *    - 只用"服务端发文本帧 + 收少量控制帧"这点功能, 引一个库要带 sha1/帧/回调
 *      一堆代码, 体积和不确定性都比自己写大;
 *    - 协议部分 (sha1/base64/帧) 已经抽成纯 C 的 ws_proto.c, 有主机端单元测试
 *      (firmware/esp32_master/test/ws_proto_test.c) 兜底;
 *    - 端口 81 与 HTTP(80) 分开, 互不影响: 网页里连不上 WS 会自动退回轮询。
 */

#ifndef WS_SERVER_H
#define WS_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 WebSocket 服务 (需要在 WiFi 就绪之后调用) */
void ws_server_begin(void);

/** 当前是否有客户端连着 (给状态页显示用) */
int ws_server_clients(void);

#ifdef __cplusplus
}
#endif

#endif /* WS_SERVER_H */
