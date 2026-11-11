#ifndef CAP_BATCH_H
#define CAP_BATCH_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define CAP_BATCH_MAX 8
#define CAP_BATCH_WIRE_MAX 72
typedef struct {uint32_t source,boot,first_seq,period_us;uint64_t first_us;uint16_t channel,unit;uint8_t type,count;uint32_t values[CAP_BATCH_MAX];} cap_batch;
size_t cap_batch_encode(const cap_batch *,uint8_t *out,size_t capacity);
bool cap_batch_decode(const uint8_t *,size_t,cap_batch *);
#endif
