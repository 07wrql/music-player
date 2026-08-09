#ifndef __AUDIO_TASK_H
#define __AUDIO_TASK_H

#include "cmsis_os.h"
#include <stdint.h>

/* 音频任务错误码 */
#define AUDIO_ERR_NONE       0   /* 无错误 */
#define AUDIO_ERR_SD_MOUNT   1   /* SD 卡挂载失败 */
#define AUDIO_ERR_NO_FILES   2   /* SD 卡上没有音频文件 */
#define AUDIO_ERR_VS1003     3   /* VS1003B 通信异常 (DREQ) */
#define AUDIO_ERR_FILE_OPEN  4   /* 音频文件打开失败 */

/* play_one() 返回值 */
#define PLAY_NEXT     0   /* 短按 → 切歌 */
#define PLAY_DONE     1   /* 播放完成 */
#define PLAY_DELETED  2   /* 长按 → 已删除, 需跳过 */

void StartAudioTask(void *argument);

/* LCD_Task 查询接口 */
extern volatile int8_t a_track;
extern volatile uint8_t a_error;
uint8_t Audio_IsPlaying(void);
uint8_t Audio_GetProgress(void);
uint8_t Audio_GetVolumeRaw(void);
int8_t  Audio_GetTrackIdx(void);
uint8_t Audio_GetError(void);    /* 获取错误码 */

#endif
