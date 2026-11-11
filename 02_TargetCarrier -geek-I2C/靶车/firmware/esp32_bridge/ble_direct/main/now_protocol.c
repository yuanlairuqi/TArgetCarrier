#include "now_protocol.h"
#include <string.h>
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
uint32_t now_crc32(const uint8_t *p, size_t n) {
    uint32_t crc=0xffffffffu;
    for (size_t i=0;i<n;++i) {
        crc^=p[i];
        for (int b=0;b<8;++b) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return crc^0xffffffffu;
}
bool now_decode(const uint8_t *d, size_t len, now_request_t *q) {
    if (!d||!q||len!=NOW_FRAME_SIZE||d[0]!='W'||d[1]!='N'||d[2]!=1||
        (d[3]!=NOW_QUERY&&d[3]!=NOW_COMMAND&&d[3]!=5)||get32(d+60)!=now_crc32(d,60)) return false;
    for (int i=d[3]==5?41:40;i<60;++i) if(d[i]) return false;
    if(d[3]==NOW_QUERY) for(int i=8;i<40;++i) if(d[i]) return false;
    memset(q,0,sizeof(*q));q->kind=d[3];q->node=get32(d+4);q->boot=get32(d+8);
    q->epoch=get32(d+12);q->session=get32(d+16);q->seq=get32(d+20);
    memcpy(q->command,d+24,d[3]==5?17:16);
    return true;
}
