#ifndef STATUS_DISPLAY_H
#define STATUS_DISPLAY_H
/* Publish only from ControlTask. All OLED IO runs in a lower-priority task. */
void StatusDisplay_Publish(void);
void StatusDisplay_Start(void);
#endif
