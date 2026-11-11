#ifndef TC_H
#define TC_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TC_SLOT 128
#define TC_CHUNK 96
#define TC_MAX 512
#define TC_Q 8
enum {
    TC_HELLO = 1,
    TC_READ = 2,
    TC_WRITE = 3,
    TC_RESULT = 4,
    TC_WAVE = 5,
    TC_STATUS = 6,
    TC_CONTROL = 7
};
enum {
    TC_OK = 0,
    TC_BAD = 1,
    TC_RANGE = 2,
    TC_UNKNOWN = 3,
    TC_BUSY = 4,
    TC_STALE = 5,
    TC_UNSUPPORTED = 6
};
enum { TC_U32 = 1, TC_I32 = 2, TC_F32 = 3 };
typedef struct {
    uint8_t type, index, count;
    uint16_t len, total;
    uint32_t session, seq, time;
    uint8_t data[TC_CHUNK];
} tc_frame;
typedef struct {
    uint8_t data[TC_SLOT];
    uint32_t at;
} tc_item;
typedef struct {
    tc_item item[TC_Q];
    uint8_t head, used;
    uint32_t dropped, peak;
} tc_queue;
typedef struct {
    uint8_t bytes[TC_SLOT];
    uint16_t used;
    uint32_t bad;
} tc_stream;
typedef struct {
    uint32_t session, seq, at;
    uint16_t total, used;
    uint8_t type, next, count;
    uint8_t data[TC_MAX];
} tc_assembly;
typedef struct {
    uint16_t id;
    uint8_t type, writable;
    uint32_t bits;
    double minimum, maximum;
} tc_param;
typedef bool (*tc_emit)(void *ctx, const uint8_t *slot, bool low, uint32_t now);
/* stage: 0 rejected, 1 parameter applied, 2 business accepted, 3 executed.
 * Stage 3 requires application evidence; transport ACK never sets it. */
typedef struct {
    uint8_t code, stage;
    uint32_t detail;
} tc_command_result;
typedef tc_command_result (*tc_command_handler)(void *ctx, uint16_t opcode, const uint8_t *data,
                                                uint16_t n);
typedef struct {
    tc_command_handler control;
    uint32_t session, last_seq, last_seen;
    bool established;
    tc_assembly rx;
    tc_param *params;
    size_t param_count;
    tc_emit emit;
    void *ctx;
    uint8_t cached[32];
    uint16_t cached_len;
    uint8_t cached_request[TC_MAX], cached_type;
    uint16_t cached_request_len;
    uint32_t duplicate, errors, reassembly_timeout;
} tc_service;
uint16_t tc_get16(const uint8_t *p);
uint32_t tc_get32(const uint8_t *p);
void tc_put16(uint8_t *p, uint16_t v);
void tc_put32(uint8_t *p, uint32_t v);
uint32_t tc_crc(const uint8_t *p, size_t n);
bool tc_encode(const tc_frame *f, uint8_t *slot);
bool tc_decode(const uint8_t *slot, tc_frame *f);
bool tc_feed(tc_stream *s, uint8_t b, uint8_t *slot);
bool tc_push(tc_queue *q, const uint8_t *slot, uint32_t now, bool drop_oldest);
bool tc_pop(tc_queue *q, uint8_t *slot, uint32_t now, uint32_t max_age);
/* Returns 1 complete, 0 pending, -1 malformed/order/timeout. Single owner task. */
int tc_join(tc_assembly *a, const tc_frame *f, uint32_t now);
bool tc_send(tc_emit emit, void *ctx, uint8_t type, uint32_t session, uint32_t seq, uint32_t time,
             const uint8_t *data, uint16_t n, bool low);
void tc_service_init(tc_service *s, tc_param *params, size_t n, tc_emit emit, void *ctx);
void tc_service_receive(tc_service *s, const uint8_t *slot, uint32_t now);
void tc_service_tick(tc_service *s, uint32_t now);
/* Wave payload: format=1, channels, sample_count, type; first_us u32,
 * period_us u32, channel_ids u16[channels], interleaved little-endian values. */
bool tc_wave(tc_emit emit, void *ctx, uint32_t session, uint32_t seq, uint32_t now_ms, uint32_t first_us,
             uint32_t period_us, const uint16_t *ids, uint8_t channels, uint8_t samples,
             uint8_t type, const void *values);
/* Acquisition and sending use separate bounded storage. One owning task, or
 * application critical section around producer/consumer calls. Never allocate. */
typedef struct {
    tc_queue frames;
    uint32_t last_sent;
} tc_wave_buffer;
bool tc_wave_buffer_emit(void *buffer, const uint8_t *slot, bool low, uint32_t now);
bool tc_wave_flush(tc_wave_buffer *buffer, tc_emit emit, void *ctx, uint32_t now,
                   uint32_t min_gap_ms);
#ifdef __cplusplus
}
#endif
#endif
