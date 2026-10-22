#include "usb_wire.h"
#include "now_protocol.h"
#include <string.h>
uint32_t wire_get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
void wire_put32(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
void usb_pack(uint8_t kind,const uint8_t body[64],uint8_t out[72]){
    out[0]='G';out[1]='U';out[2]=1;out[3]=kind;memcpy(out+4,body,64);wire_put32(out+68,now_crc32(out,68));
}
bool usb_feed(usb_parser_t *p,uint8_t b,uint8_t body[64],uint8_t *kind){
    p->bytes[p->used++]=b;
    if(p->used<72)return false;
    if(p->bytes[0]=='G'&&p->bytes[1]=='U'&&p->bytes[2]==1&&wire_get32(p->bytes+68)==now_crc32(p->bytes,68)){
        memcpy(body,p->bytes+4,64);*kind=p->bytes[3];p->used=0;return true;
    }
    memmove(p->bytes,p->bytes+1,71);p->used=71;return false;
}
bool master_output_confirmed(const uint8_t s[64],const uint8_t c[64]){
    if(s[3]!=3||c[3]!=2||s[24]!=0||s[25]!=2||!wire_get32(c+16)||memcmp(s+4,c+4,16))return false;
    uint32_t seq=wire_get32(c+20);
    const uint8_t *r=s+40,*cmd=c+24;
    bool hardware=(r[2]&0xb8)==0xb8&&!(r[2]&0x42)&&r[7]==0;
    if(!hardware||wire_get32(r+8)!=seq||wire_get32(r+12)!=seq||r[5]!=cmd[1]||r[6]!=cmd[2]||r[18]!=cmd[3])return false;
    return cmd[3]==1||(r[1]==cmd[1]&&r[19]==cmd[1]);
}
