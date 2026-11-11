#ifndef TC_RECORDER_H
#define TC_RECORDER_H
#include "capture.h"
typedef enum {REC_IDLE,REC_ACTIVE,REC_IO_ERROR,REC_OVERFLOW} rec_state;
typedef bool (*rec_write_fn)(void *,const uint8_t *,size_t);
typedef struct {cap_queue queue;rec_state state;uint32_t written;rec_write_fn write;void *context;} recorder;
/* Caller serializes queue access. drain may block; call only from storage task. */
void rec_init(recorder *,rec_write_fn,void *);
bool rec_start(recorder *);
bool rec_submit(recorder *,const cap_record *);
bool rec_drain(recorder *,unsigned);
bool rec_stop(recorder *);
#endif
