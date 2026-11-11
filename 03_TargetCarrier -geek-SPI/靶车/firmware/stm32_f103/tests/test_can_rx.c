#include "dm_motor.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)

int main(void)
{
    dm_feedback_t fb = {0};
    dm_param_reply_t param = {0};
    /* Tail bytes reproduce the rid=127/128, raw=e7ff1b18 / 07ff1b18
     * patterns in the user's log. These are velocity/torque/temperature,
     * not register addresses or values. */
    uint8_t motor[8] = {0x01,0x7f,0xff,0x7f,0xe7,0xff,0x1b,0x18};
    CHECK(dm_decode_can(1,0,0,motor,8,&fb,&param)==DM_RX_FEEDBACK);
    CHECK(fb.id==1 && fb.err==0 && fb.temp_mos==27 && fb.temp_coil==24);
    CHECK(fabsf(fb.pos_rad)<0.001f);
    CHECK(fabsf(fb.vel_rad_s)<0.04f);
    motor[3]=0x80;motor[4]=0x07;
    CHECK(dm_decode_can(1,0,0,motor,8,&fb,&param)==DM_RX_FEEDBACK);
    CHECK(fb.vel_rad_s>0 && fb.vel_rad_s<0.04f);
    motor[0]=0x11;
    CHECK(dm_decode_can(1,0,0,motor,8,&fb,&param)==DM_RX_FEEDBACK && fb.err==1);
    motor[0]=0xa1;
    CHECK(dm_decode_can(1,0,0,motor,8,&fb,&param)==DM_RX_FEEDBACK && fb.err==0xa);
    motor[0]=1;
    CHECK(dm_decode_can(1,0,1,motor,8,&fb,&param)==DM_RX_FEEDBACK);
    CHECK(dm_decode_can(1,0x12,0x12,motor,8,&fb,&param)==DM_RX_FEEDBACK);
    CHECK(dm_decode_can(1,0,2,motor,8,&fb,&param)==DM_RX_IGNORE);
    CHECK(dm_decode_can(1,0,0x7ff,motor,8,&fb,&param)==DM_RX_IGNORE);
    motor[0]=2;
    CHECK(dm_decode_can(1,0,0,motor,8,&fb,&param)==DM_RX_IGNORE);

    uint8_t reg[8]={1,0,0x33,7,0x78,0x56,0x34,0x12};
    const uint8_t ops[]={0x33,0x55,0xaa};
    for (unsigned i=0;i<sizeof(ops);++i) {
        reg[2]=ops[i];
        CHECK(dm_decode_can(1,0,0,reg,8,&fb,&param)==DM_RX_PARAM_REPLY);
        CHECK(param.rid==7 && memcmp(param.value,reg+4,4)==0);
        CHECK(dm_decode_can(1,0,0x7ff,reg,8,&fb,&param)==DM_RX_PARAM_REPLY);
    }
    reg[0]=2;
    CHECK(dm_decode_can(1,0,0,reg,8,&fb,&param)==DM_RX_IGNORE);
    reg[0]=1;reg[1]=0x7f;reg[2]=0x33; /* Opcode alone is not a register reply. */
    CHECK(dm_decode_can(1,0,0,reg,8,&fb,&param)==DM_RX_FEEDBACK);
    CHECK(dm_decode_can(1,0,0x7ff,reg,8,&fb,&param)==DM_RX_IGNORE);
    for (unsigned n=0;n<8;++n) CHECK(dm_decode_can(1,0,0,motor,(uint8_t)n,&fb,&param)==DM_RX_IGNORE);
    CHECK(dm_decode_can(1,0,0,motor,9,&fb,&param)==DM_RX_IGNORE);
    CHECK(dm_decode_can(1,0,0,NULL,8,&fb,&param)==DM_RX_IGNORE);
    CHECK(dm_decode_can(1,0,0,motor,8,NULL,&param)==DM_RX_IGNORE);
    CHECK(dm_decode_can(1,0,0x800,motor,8,&fb,&param)==DM_RX_IGNORE);

    /* Exhaust all position encodings for enabled and fault feedback. The
     * status nibble and raw fault must survive routing, including op bytes. */
    const uint8_t states[]={0x11,0x81,0xa1};
    for (unsigned s=0;s<sizeof(states);++s) {
        motor[0]=states[s];
        for (unsigned p=0;p<65536;++p) {
            motor[1]=(uint8_t)(p>>8);motor[2]=(uint8_t)p;
            CHECK(dm_decode_can(1,0,0,motor,8,&fb,&param)==DM_RX_FEEDBACK);
            CHECK(fb.err==(states[s]>>4));
        }
    }
    printf("PASS CAN receive routing: %u assertions; feedback, register replies, faults, IDs, malformed frames\n",checks);
    return 0;
}
