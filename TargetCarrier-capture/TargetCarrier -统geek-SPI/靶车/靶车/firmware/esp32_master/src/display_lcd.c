/**
 * @file    display_lcd.c
 * @brief   主机 ESP32-S3-GEEK 板载 ST7789 状态屏 (135x240, 10x14 清晰字模)
 *
 *  这次换的是"像素清晰度": 之前用 3x5 字模放大 2 倍 (6x10), 笔画粗、边缘糊。
 *  现在直接用 10x14 点阵 (lcd_font10x14.h, 由 Consolas Bold 生成), 屏上 1:1 绘制,
 *  每个像素就是屏上的一个像素, 边缘干净; 每行 12 个字符, 一共 9 行。
 *
 *  方向/颜色参数是实机验证过的那组, 不要动:
 *      swap=0, mirror(true,false), gap=(52,40), BGR, invert=true
 */

#include "display_lcd.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#include "lcd_font10x14.h"

/* ---- 板载屏引脚 (Waveshare ESP32-S3-GEEK) ---- */
#define PIN_SCLK   12
#define PIN_MOSI   11
#define PIN_CS     10
#define PIN_DC      8
#define PIN_RST     9
#define PIN_BL      7

#define PANEL_W    135
#define PANEL_H    240

#define GAP_X      52
#define GAP_Y      40

#define LINE_H     18
#define LINE_PITCH 18
#define LINE_TOP   8
#define LCD_LINE_MAX 12

#define COL_BG     0x0861
#define COL_FG     0xFFFF
#define COL_ACCENT 0x07E0
#define COL_WARN   0xFD20

static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_ready = false;
static uint16_t s_line[PANEL_W * LINE_H];

/* ------------------------------------------------------------------ */
/* 背光                                                               */
/* ------------------------------------------------------------------ */

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = 5000,
    };
    ledc_timer_config(&t);

    ledc_channel_config_t c = {
        .gpio_num   = PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&c);
}

static void backlight_set(uint8_t percent)
{
    uint32_t duty = (uint32_t)(percent * 255u / 100u);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

/* ------------------------------------------------------------------ */
/* 初始化                                                             */
/* ------------------------------------------------------------------ */

bool lcd_init(void)
{
    backlight_init();

    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .sclk_io_num     = PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = PANEL_W * LINE_H * sizeof(uint16_t),
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return false;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = PIN_CS,
        .dc_gpio_num       = PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io) != ESP_OK) {
        return false;
    }

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = PIN_RST,
        .color_space    = ESP_LCD_COLOR_SPACE_BGR,
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &dev, &s_panel) != ESP_OK) {
        return false;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    esp_lcd_panel_set_gap(s_panel, GAP_X, GAP_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

    backlight_set(80);
    s_ready = true;
    lcd_clear();
    return true;
}

/* ------------------------------------------------------------------ */
/* 填充 / 清底                                                        */
/* ------------------------------------------------------------------ */

void lcd_fill(uint16_t color)
{
    if (!s_ready) {
        return;
    }
    for (int i = 0; i < PANEL_W * LINE_H; i++) {
        s_line[i] = color;
    }
    for (int y = 0; y < PANEL_H; y += LINE_H) {
        int y1 = (y + LINE_H > PANEL_H) ? PANEL_H : (y + LINE_H);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, PANEL_W, y1, s_line);
        vTaskDelay(1);
    }
}

void lcd_clear(void)
{
    lcd_fill(COL_BG);
}

/* ------------------------------------------------------------------ */
/* 文字: 10x14 字模, 1:1 绘制                                         */
/* ------------------------------------------------------------------ */

static void line_render(const char *text, uint16_t fg)
{
    for (int i = 0; i < PANEL_W * LINE_H; i++) {
        s_line[i] = COL_BG;
    }
    if (text == NULL) {
        return;
    }

    int x = 2;
    int y = (LINE_H - LCDF_H) / 2;

    for (const char *p = text; *p != '\0' && x + LCDF_W <= PANEL_W; p++) {
        const lcdf_glyph_t *g = lcdf_lookup(*p);
        for (int row = 0; row < LCDF_H; row++) {
            uint16_t bits = g->rows[row];
            if (bits == 0) {
                continue;
            }
            int yy = y + row;
            if (yy < 0 || yy >= LINE_H) {
                continue;
            }
            for (int col = 0; col < LCDF_W; col++) {
                if ((bits >> (LCDF_W - 1 - col)) & 1u) {
                    int xx = x + col;
                    if (xx >= 0 && xx < PANEL_W) {
                        s_line[yy * PANEL_W + xx] = fg;
                    }
                }
            }
        }
        x += LCDF_ADV;
    }
}

static void draw_line(int index, const char *text, uint16_t fg)
{
    if (!s_ready || index < 0 || index >= LCD_LINE_MAX) {
        return;
    }
    line_render(text, fg);
    int y0 = LINE_TOP + index * LINE_PITCH;
    if (y0 + LINE_H > PANEL_H) {
        return;
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, PANEL_W, y0 + LINE_H, s_line);
}

void lcd_show_lines(const char *const *lines, int count)
{
    static const uint16_t colors[3] = { COL_ACCENT, COL_FG, COL_WARN };
    if (!s_ready) {
        return;
    }
    for (int i = 0; i < count && i < LCD_LINE_MAX; i++) {
        draw_line(i, lines[i], colors[i < 3 ? i : 1]);
    }
}

void lcd_show(const char *l1, const char *l2, const char *l3,
              const char *l4, const char *l5)
{
    const char *lines[5] = { l1, l2, l3, l4, l5 };
    lcd_show_lines(lines, 5);
}
