#include <stddef.h>
typedef struct mock_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned,unsigned);
int xQueueSend(QueueHandle_t,const void*,unsigned);
int xQueueReceive(QueueHandle_t,void*,unsigned);
unsigned uxQueueSpacesAvailable(QueueHandle_t);
void xQueueReset(QueueHandle_t);
