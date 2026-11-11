#include "oled_text.h"
#include <string.h>
/* Small ASCII diagnostic font. Each value is one vertical column, bit 0 at top. */
static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-:./%?=";
static const uint8_t glyphs[][5] = {
 {62,81,73,69,62},{0,66,127,64,0},{66,97,81,73,70},{33,65,69,75,49},
 {24,20,18,127,16},{39,69,69,69,57},{60,74,73,73,48},{1,113,9,5,3},
 {54,73,73,73,54},{6,73,73,41,30},
 {126,17,17,17,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
 {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,122},{127,8,8,8,127},
 {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
 {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
 {7,8,112,8,7},{97,81,73,69,67},
 {8,8,8,8,8},{0,54,54,0,0},{0,96,96,0,0},{32,16,8,4,2},
 {35,19,8,100,98},{2,1,81,9,6},{20,20,20,20,20}
};
static bool valid(const oled_text_t *d)
{
    return d && d->write && (d->address == 0x3c || d->address == 0x3d) &&
           (d->height == 32 || d->height == 64) && d->column_offset <= 4;
}
bool oled_text_init(const oled_text_t *d)
{
    if (!valid(d)) return false;
    const uint8_t init[] = {0x00,0xae,0xd5,0x80,0xa8,(uint8_t)(d->height-1),
        0xd3,0x00,0x40,0xa1,0xc8,0xda,(uint8_t)(d->height==64?0x12:0x02),
        0x81,0x7f,0xd9,(uint8_t)(d->sh1106?0x22:0xf1),0xdb,0x40,0xa4,0xa6};
    if (!d->write(d->ctx,d->address,init,sizeof(init))) return false;
    const uint8_t ssd[] = {0x00,0x20,0x02,0x8d,0x14}; /* Page mode, internal pump. */
    const uint8_t sh[] = {0x00,0xad,0x8b};
    return d->write(d->ctx,d->address,d->sh1106?sh:ssd,d->sh1106?sizeof(sh):sizeof(ssd));
}
bool oled_text_frame(const oled_text_t *d, const char lines[8][22], unsigned bank)
{
    if (!valid(d) || !lines) return false;
    unsigned pages = d->height/8U;
    unsigned first = d->height==32 ? (bank%2U)*4U : 0U;
    for (unsigned p=0;p<pages;p++) {
        uint8_t row[128] = {0};
        for (unsigned c=0;c<OLED_TEXT_COLS && lines[first+p][c];c++) {
            char ch=lines[first+p][c];
            if (ch>='a' && ch<='z') ch=(char)(ch-'a'+'A');
            const char *g=strchr(alphabet,ch);
            if (g) memcpy(row+c*6U,glyphs[g-alphabet],5);
        }
        uint8_t cmd[] = {0x00,(uint8_t)(0xb0U+p),(uint8_t)(d->column_offset&15U),
                         (uint8_t)(0x10U|(d->column_offset>>4))};
        if (!d->write(d->ctx,d->address,cmd,sizeof(cmd))) return false;
        for (unsigned col=0;col<128;col+=16) {
            uint8_t packet[17];packet[0]=0x40;memcpy(packet+1,row+col,16);
            if (!d->write(d->ctx,d->address,packet,sizeof(packet))) return false;
        }
    }
    const uint8_t on[] = {0x00,0xaf};
    return d->write(d->ctx,d->address,on,sizeof(on));
}
