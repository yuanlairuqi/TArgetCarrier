#include "oled_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned calls,fail_at,pages,rows,display_on;
static uint8_t image[8][128],page,column,offset;
static bool send(void *ctx,uint8_t addr,const uint8_t *b,size_t n)
{
    (void)ctx;assert(addr==0x3c || addr==0x3d);assert(n>1 && n<=24);
    if(++calls==fail_at) return false;
    if(b[0]==0 && n==4 && (b[1]&0xf8)==0xb0){
        page=b[1]&7;column=0;offset=(b[2]&15)|((b[3]&15)<<4);++pages;
    }else if(b[0]==0x40){
        assert(n==17 && column<=112);memcpy(image[page]+column,b+1,16);column+=16;++rows;
    }else if(n==2 && b[1]==0xaf) ++display_on;
    return true;
}
static void reset(void){calls=pages=rows=display_on=0;fail_at=0;memset(image,0xcc,sizeof(image));}
int main(void)
{
    oled_text_t d={.write=send,.address=0x3c,.height=64};
    char lines[8][22]={{0}};strcpy(lines[0],"1");strcpy(lines[4],"2");
    reset();assert(oled_text_init(&d));assert(oled_text_frame(&d,(const char (*)[22])lines,0));
    assert(pages==8 && rows==64 && display_on==1 && offset==0);
    const uint8_t one[]={0,66,127,64,0};assert(!memcmp(image[0],one,5));
    for(unsigned i=6;i<128;i++)assert(image[0][i]==0);
    for(unsigned i=0;i<128;i++)assert(image[1][i]==0);
    puts("PASS full 128x64 frame and cleared trailing pixels");
    reset();d.height=32;assert(oled_text_frame(&d,(const char (*)[22])lines,1));
    const uint8_t two[]={66,97,81,73,70};assert(!memcmp(image[0],two,5));assert(pages==4 && rows==32);
    puts("PASS 128x32 second-bank pagination");
    reset();d.sh1106=true;d.column_offset=2;d.address=0x3d;
    assert(oled_text_init(&d));assert(oled_text_frame(&d,(const char (*)[22])lines,0));assert(offset==2);
    puts("PASS SH1106 offset and alternate address");
    reset();fail_at=1;assert(!oled_text_init(&d));assert(calls==1);
    reset();fail_at=4;assert(!oled_text_frame(&d,(const char (*)[22])lines,0));assert(calls==4 && display_on==0);
    puts("PASS absent device and mid-frame IO failure stop immediately");
    reset();d.height=48;assert(!oled_text_init(&d));assert(!oled_text_frame(&d,(const char (*)[22])lines,0));assert(calls==0);
    d.height=64;d.address=0x78;assert(!oled_text_init(&d));assert(calls==0);
    puts("PASS invalid geometry and shifted address rejected before IO");
    reset();d.address=0x3c;for(unsigned r=0;r<8;r++)for(unsigned c=0;c<21;c++)lines[r][c]=(char)(32+r*10+c);
    assert(oled_text_frame(&d,(const char (*)[22])lines,0));assert(rows==64);
    puts("PASS maximum text width bounded");
}
