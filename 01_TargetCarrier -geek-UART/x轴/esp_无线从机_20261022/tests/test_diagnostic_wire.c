#include <assert.h>
#include <stdio.h>
#include "diagnostic_wire.h"
int main(void){diagnostic_state_t d={0};robot_control_t r={0};robot_bridge_t b={0};
 b.online=true;b.last_rx=900;b.peer_flags=33;b.peer_phase=3;b.peer_error=1;b.error=7;r.error=7;r.motion=0;
 diagnostic_update(&d,&r,&b,1000,120,2,1);assert(d.bytes[0]==1&&d.bytes[3]==1&&d.bytes[16]==100);
 b.peer_error=0;b.last_rx=1100;diagnostic_update(&d,&r,&b,1100,150,0,1);assert(d.bytes[3]==1&&d.bytes[12]==120);
 r.error=0;b.error=0;diagnostic_update(&d,&r,&b,1200,150,0,1);assert(d.bytes[3]==1&&!d.fault_active);
 FILE *f=fopen("diagnostic_frame.bin","wb");assert(f);assert(fwrite(d.bytes,1,20,f)==20);fclose(f);
 r.error=7;b.error=7;b.peer_error=2;diagnostic_update(&d,&r,&b,1300,180,0,1);assert(d.bytes[3]==2&&d.bytes[12]==180);
 puts("PASS first fault survives later STOP replies; a new fault captures a new cause");}
