"""Compile real GPIO limit driver against host stubs; no hardware access."""
from pathlib import Path
import os, subprocess, argparse, tempfile
p=argparse.ArgumentParser();p.add_argument('--gcc',required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[1]
work=Path(tempfile.mkdtemp(prefix='limit-host-'))
headers={
'freertos/FreeRTOS.h': '''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int portMUX_TYPE; typedef int BaseType_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
#define pdFALSE 0
#define portYIELD_FROM_ISR() ((void)0)
''',
'freertos/task.h': '''#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
extern unsigned notified;
static inline void vTaskNotifyGiveFromISR(TaskHandle_t t,BaseType_t *w){(void)t;(void)w;++notified;}
static inline void xTaskNotifyGive(TaskHandle_t t){(void)t;++notified;}
''',
'driver/gpio.h': '''#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_NEGEDGE 2
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
int gpio_get_level(int pin);
esp_err_t gpio_config(const gpio_config_t *c);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(int pin,void (*cb)(void *),void *arg);
const char *esp_err_to_name(int err);
''',
'sdkconfig.h':'#pragma once\n',
'esp_attr.h':'#define IRAM_ATTR\n',
'esp_intr_alloc.h':'#define ESP_INTR_FLAG_IRAM 1024\n',
'esp_log.h':'#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n',
}
for name,data in headers.items():
    dest=work/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_text(data)
test=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "limit_inputs.h"
#include "driver/gpio.h"
unsigned notified;static int levels[3]={1,1,1},fail;
static void (*handlers[3])(void *);
int gpio_get_level(int pin){return levels[pin];}
int gpio_config(const gpio_config_t *c){
 assert(c->pin_bit_mask==6 && c->mode==GPIO_MODE_INPUT);
 assert(c->pull_up_en && !c->pull_down_en && c->intr_type==GPIO_INTR_NEGEDGE);return fail;
}
int gpio_install_isr_service(int flags){assert(flags==1024);return 0;}
int gpio_isr_handler_add(int pin,void (*cb)(void *),void *arg){(void)arg;handlers[pin]=cb;return 0;}
const char *esp_err_to_name(int err){(void)err;return "stub";}
int main(int argc,char **argv){
 assert(argc==2);int n=atoi(argv[1]);
 if(n==0){assert(limit_inputs_blocked());puts("PASS uninitialized input inhibits movement");return 0;}
 if(n>=8){
  assert(limit_inputs_init((void *)1));
  levels[1]=0;handlers[1](NULL);assert(limit_inputs_blocked());
  levels[1]=1;limit_inputs_sample(1000);assert(!limit_inputs_try_reset(1099));
  if(n==8){assert(limit_inputs_try_reset(1100));assert(!limit_inputs_blocked());}
  if(n==9){levels[2]=0;limit_inputs_sample(1100);assert(!limit_inputs_try_reset(1200));assert(limit_inputs_blocked());}
  if(n==10){handlers[2](NULL);assert(!limit_inputs_try_reset(1200));limit_inputs_sample(1200);assert(!limit_inputs_try_reset(1299));assert(limit_inputs_try_reset(1300));}
  if(n==11){limit_inputs_sample(0xfffffff0U); /* reset tracking by an edge first */
   handlers[1](NULL);limit_inputs_sample(0xfffffff0U);assert(!limit_inputs_try_reset(0x53U));assert(limit_inputs_try_reset(0x54U));}
  printf("PASS reset case %d (100ms release / low inhibit / ISR race / wrap)\n",n);return 0;
 }
 if(n==1||n==2)levels[n]=0;
 if(n==7)fail=1;
 assert(limit_inputs_init((void *)1)==(n!=7));
 if(n==1||n==2||n==7){assert(limit_inputs_blocked());levels[1]=levels[2]=1;assert(limit_inputs_blocked());}
 else {
  assert(!limit_inputs_blocked());int pin=(n==4||n==6)?2:1;levels[pin]=0;
  if(n==3||n==4){unsigned before=notified;handlers[pin](NULL);assert(notified>before);levels[pin]=1;}
  assert(limit_inputs_blocked());levels[pin]=1;assert(limit_inputs_blocked());
 }
 printf("PASS GPIO case %d (boot low / ISR short pulse / polled low / setup failure)\n",n);return 0;
}
'''
(work/'test.c').write_text(test)
env=dict(os.environ,PATH=str(Path(a.gcc).parent)+os.pathsep+os.environ.get('PATH',''))
subprocess.run([a.gcc,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(work),'-I'+str(root/'main'),str(work/'test.c'),str(root/'main/limit_inputs.c'),'-o',str(work/'test.exe')],check=True,env=env)
for n in range(12):subprocess.run([str(work/'test.exe'),str(n)],check=True,env=env)
