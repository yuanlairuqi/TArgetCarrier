#include "recorder.h"
#include <string.h>
void rec_init(recorder*r,rec_write_fn fn,void*ctx){memset(r,0,sizeof(*r));r->write=fn;r->context=ctx;}
bool rec_start(recorder*r){if(!r||!r->write||r->state!=REC_IDLE)return false;r->state=REC_ACTIVE;return true;}
bool rec_submit(recorder*r,const cap_record*x){uint8_t checked[CAPTURE_WIRE_SIZE];if(!r||r->state!=REC_ACTIVE||!cap_encode(x,checked))return false;if(!cap_push(&r->queue,x)){r->state=REC_OVERFLOW;return false;}return true;}
bool rec_drain(recorder*r,unsigned max){if(!r||(r->state!=REC_ACTIVE&&r->state!=REC_OVERFLOW))return false;while(max--&&r->queue.count){uint8_t buf[CAPTURE_WIRE_SIZE];cap_record *x=&r->queue.items[r->queue.head];if(!cap_encode(x,buf)||!r->write(r->context,buf,sizeof(buf))){r->state=REC_IO_ERROR;return false;}cap_record done;cap_pop(&r->queue,&done);r->written++;}return true;}
bool rec_stop(recorder*r){if(!r||(r->state!=REC_ACTIVE&&r->state!=REC_OVERFLOW)||r->queue.count)return false;r->state=REC_IDLE;return true;}
