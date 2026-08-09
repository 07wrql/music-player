/**
 * @file    lcd_task.c
 * @brief   LCD 显示 — 直接渲染, 字库读失败快速跳过, 下次重试
 */
#include "lcd_task.h"
#include "audio_task.h"
#include "sd_task.h"
#include "lcd.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

extern lcd lcd_desc;
extern volatile int8_t a_track;  /* 直接读, 不用函数 */
osSemaphoreId_t lcdWakeSem = NULL;

void StartLCDTask(void *argument)
{
  (void)argument;
  char buf[64];

  lcd_init_dev(&lcd_desc, LCD_1_47_INCH, LCD_ROTATE_90);
  lcd_clear(&lcd_desc, BLACK);
  lcd_print(&lcd_desc, 100, 70, "VS1003B Player");

  while (!SD_IsMounted()) osDelay(200);

  for (;;) {
    lcd_clear(&lcd_desc, BLACK);

    uint8_t cnt     = SD_GetTrackCount();
    uint8_t playing = Audio_IsPlaying();
    int8_t track    = a_track;  /* 直接读 volatile */
    uint8_t err     = Audio_GetError();

    if (err != AUDIO_ERR_NONE) {
      snprintf(buf, sizeof(buf), "ERR: %d", err);
      lcd_print(&lcd_desc, 100, 70, buf);
      osDelay(1000);
      continue;
    }

    if (track < 0) {
      snprintf(buf, sizeof(buf), "Ready  [%d tracks]", cnt);
      lcd_print(&lcd_desc, 60, 70, buf);
      osDelay(500);
      continue;
    }

    /* ---- 歌名 (track>=0 就显示) ---- */
    {
      const char *nm = SD_GetTrackName((uint8_t)track);
      if (nm) {
        uint16_t w = gbk_str_width((const uint8_t*)nm);
        if (w > 300) w = 300;
        uint16_t cx = (320 - w) / 2;
        lcd_set_font(&lcd_desc, FONT_1608, LIGHTBLUE, BLACK);
        lcd_show_gbk_string(&lcd_desc, cx, 40, (const uint8_t*)nm);
      }
    }

    /* ---- 曲目 (始终显示, 不受字库影响) ---- */
    {
      lcd_set_font(&lcd_desc, FONT_1608, CYAN, BLACK);
      snprintf(buf, sizeof(buf), "%d / %d", track + 1, cnt);
      lcd_print(&lcd_desc, 130, 80, buf);
    }

    osDelay(500);
  }
}
