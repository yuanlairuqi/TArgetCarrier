#include <assert.h>
#include <stdio.h>
#include "../main/pulse_capture.c"
static int64_t fake_us; static int nulls,aborts,sends,allow; static int64_t times[16];
int64_t esp_timer_get_time(void){return fake_us;}
int gpio_get_level(int p){(void)p;return 0;}
struct os_mbuf *ble_hs_mbuf_from_flat(const void*p,size_t n){(void)p;(void)n;return allow?(struct os_mbuf*)1:0;}
int ble_gatts_notify_custom(unsigned short c,unsigned short h,struct os_mbuf*m){(void)c;(void)h;(void)m;times[sends++]=fake_us;return 0;}
void notify_diag_pulse_mbuf_null(void){nulls++;}
void notify_diag_pulse_abort(void){aborts++;}
void notify_diag_pulse_fail(int rc){(void)rc;}
void notify_diag_pulse_ok(void){}
int main(void){previous_conn=1;state=2;transfer_us=1;started_us=1;fake_us=100000; snapshot.count=6; snapshot.duration=1000;
for(int i=0;i<10;i++){pulse_capture_poll(1,1,true);fake_us+=10000;}
assert(nulls==1 && aborts==1 && state==0);
allow=1;state=2;ble_next_us=0;fake_us=100000;transfer_us=1;started_us=1;packet_index=0;
for(int i=0;i<11;i++){pulse_capture_poll(1,1,true);fake_us+=10000;}
assert(sends==4 && state==0);for(int i=1;i<sends;i++)assert(times[i]-times[i-1]>=30000);
printf("PASS: one abort on failure; four packets spaced at least 30 ms; no retry storm\n");return 0;}
