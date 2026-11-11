#ifndef TC_CAPTURE_H
#define TC_CAPTURE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CAPTURE_WIRE_SIZE 40u
#define CAPTURE_QUEUE_SIZE 32u
typedef enum {CAP_SAMPLE=1,CAP_STATE=2,CAP_EVENT=3} cap_kind;
typedef enum {CAP_U32=1,CAP_I32=2,CAP_F32=3} cap_type;
typedef struct {uint8_t kind,type;uint16_t channel,unit;uint32_t source,boot,seq;uint64_t sample_us;uint32_t value;} cap_record;
typedef struct {cap_record items[CAPTURE_QUEUE_SIZE];uint16_t head,count;uint32_t dropped,high_water;} cap_queue;
/* Single owner, or externally synchronize calls. No allocation, no blocking. */
bool cap_encode(const cap_record *,uint8_t *out);
bool cap_decode(const uint8_t *in,cap_record *);
bool cap_push(cap_queue *,const cap_record *);
bool cap_pop(cap_queue *,cap_record *);
#endif
