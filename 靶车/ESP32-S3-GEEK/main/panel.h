#pragma once
#include <stdbool.h>
#include <stdint.h>
void panel_init(void);
void panel_draw(bool connected, bool armed, int value, const char *text, uint32_t rgb, int brightness);
