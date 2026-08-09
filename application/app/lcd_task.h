#ifndef __LCD_TASK_H
#define __LCD_TASK_H

#include "cmsis_os.h"

extern osSemaphoreId_t lcdWakeSem;   /* Audio 释放此信号量唤醒 LCD 渲染 */

void StartLCDTask(void *argument);
void LCD_CacheSongName(const char *name);  /* Audio 切歌时调用, 预读字库 */

#endif
