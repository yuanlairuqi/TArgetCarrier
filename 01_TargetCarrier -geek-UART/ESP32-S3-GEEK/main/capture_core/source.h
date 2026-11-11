#ifndef CAP_SOURCE_H
#define CAP_SOURCE_H
#include "capture.h"
#include "fragment.h"
#ifndef CAP_SNAPSHOT_PERIOD_MS
#define CAP_SNAPSHOT_PERIOD_MS 500U
#endif
typedef bool (*cap_send_fn)(const uint8_t fragment[20],void *ctx);
/* Single application-task owner. Call feed for actual samples only. Never invent a
 * fixed-rate timestamp from a delayed scheduler. Zero source/boot are invalid. */
typedef struct {cap_queue queue;uint32_t source,boot,seq,batch,last_send,started;
 unsigned index,total;bool pending;uint8_t wire[40];uint32_t lost,reported;} cap_source;
void cap_source_init(cap_source *,uint32_t source,uint32_t boot);
bool cap_source_feed(cap_source *,uint8_t kind,uint16_t channel,uint8_t type,
 uint16_t unit,uint64_t actual_us,uint32_t value);
/* At most one fragment/40 ms; shares existing lowest-priority wireless queue. */
void cap_source_poll(cap_source *,bool active,uint64_t now_us,cap_send_fn,void *ctx);
#endif
