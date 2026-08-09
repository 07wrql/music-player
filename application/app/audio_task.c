/**
 * @file    audio_task.c
 * @brief   音频播放任务 — sd_sync 互斥锁保护, LCD/音频安全共存
 *
 * sd_sync 内部用 osMutex 串行化所有 SD 访问, 防止 LCD 读字库与音频读数据竞争。
 * VS1003B 内部 2048 字节 FIFO ≈ 10.7ms 缓冲 (48kHz stereo)。
 */
#include "audio_task.h"
#include "sd_task.h"
#include "lcd_task.h"
#include "vs1003b.h"
#include "main.h"
#include <string.h>

/* 音频缓冲 1KB, sd_sync 互斥锁保护多任务安全 */

/* 播放状态（LCD_Task 读取） */
static volatile uint8_t  a_playing  = 0;
static volatile uint8_t  a_progress = 0;
static volatile uint8_t  a_volume   = 0xCC   /* 20% 音量 */;
volatile int8_t   a_track    = -1;
volatile uint8_t  a_error    = AUDIO_ERR_NONE;
static uint32_t a_total = 0, a_sent = 0;

uint8_t Audio_IsPlaying(void)    { return a_playing; }
uint8_t Audio_GetProgress(void)  { return a_progress; }
uint8_t Audio_GetVolumeRaw(void) { return a_volume; }
int8_t  Audio_GetTrackIdx(void)  { return a_track; }
uint8_t Audio_GetError(void)     { return a_error; }

/* sd_sync 声明在 sd_task.h (含互斥锁保护), 实现在 sd_task.c */

/**
 * @brief  发送 32 字节零块 x64 次, 排空 VS1003B 解码残留
 */
static void send_zeros(void)
{
    uint8_t z[32];
    memset(z, 0, 32);
    for (int i = 0; i < 64; i++) {
        if (!VS1003_WaitDREQ(100)) break;
        VS1003_SendData(z, 32);
    }
}

/**
 * @brief  流式播放单个文件 (同步读取, sd_sync 内部互斥锁保证多任务安全)
 * @retval PLAY_NEXT=0(切歌) PLAY_DONE=1(播完) PLAY_DELETED=2(删除)
 */
static uint8_t play_one(const char* filename)
{
    uint8_t  btn_prev    = GPIO_PIN_SET;
    uint32_t btn_down_at = 0;

    /* ---- 打开文件 ---- */
    if (sd_sync(SD_CMD_OPEN, filename, NULL, 0, 0) != FR_OK) {
        a_error = AUDIO_ERR_FILE_OPEN;
        return PLAY_DONE;
    }
    a_total = sd_sync(SD_CMD_GET_SIZE, NULL, NULL, 0, 0);
    a_sent  = 0;
    a_progress = 0;
    sd_sync(SD_CMD_LSEEK, NULL, NULL, 0, 0);

    /* ---- 复位 VS1003B (先软复位, 失败则硬复位) ---- */
    if (!VS1003_SoftReset()) {
        /* 软复位失败通常是因为上一首 send_zeros 后芯片状态异常,
         * 硬件复位 (拉 XRESET) 可以强制恢复到已知状态 */
        Mp3PutInReset();
        HAL_Delay(20);
        Mp3DeselectControl();
        Mp3DeselectData();
        Mp3ReleaseFromReset();
        HAL_Delay(200);
        if (!VS1003_SoftReset()) {
            a_error = AUDIO_ERR_VS1003;
            sd_sync(SD_CMD_CLOSE, NULL, NULL, 0, 0);
            return PLAY_DONE;
        }
    }
    {
        uint16_t m = Mp3ReadRegister(SPI_MODE);
        if (m == 0xFFFF) {
            a_error = AUDIO_ERR_VS1003;
            sd_sync(SD_CMD_CLOSE, NULL, NULL, 0, 0);
            return PLAY_DONE;
        }
        m |= SM_STREAM;
        Mp3WriteRegister(SPI_MODE, (m >> 8) & 0xFF, m & 0xFF);
    }

    a_error   = AUDIO_ERR_NONE;
    a_playing = 1;

    /* ---- 同步播放循环 ---- */
    static uint8_t audio_buf[2048];   /* 2KB, 减少 SD 访问频率 */

    for (;;) {
        /* ---- 按键检测 (短按切歌, 长按≥2秒删除) ---- */
        {
            uint8_t  btn = HAL_GPIO_ReadPin(BTN_NEXT_GPIO_Port, BTN_NEXT_Pin);
            uint32_t now = HAL_GetTick();

            if (btn_prev == GPIO_PIN_SET && btn == GPIO_PIN_RESET) {
                btn_down_at = now;
            }
            if (btn_prev == GPIO_PIN_RESET && btn == GPIO_PIN_SET) {
                if (btn_down_at && (now - btn_down_at < 2000)) {
                    a_playing = 0;
                    send_zeros();
                    sd_sync(SD_CMD_CLOSE, NULL, NULL, 0, 0);
                    return PLAY_NEXT;
                }
                btn_down_at = 0;
            }
            if (btn == GPIO_PIN_RESET && btn_down_at
                && (now - btn_down_at >= 2000)) {
                btn_down_at = 0;
                a_playing   = 0;
                send_zeros();
                sd_sync(SD_CMD_CLOSE, NULL, NULL, 0, 0);
                sd_sync(SD_CMD_DELETE, filename, NULL, 0, 0);
                sd_sync(SD_CMD_MOUNT, NULL, NULL, 0, 0);
                return PLAY_DELETED;
            }
            btn_prev = btn;
        }

        /* ---- 等 VS1003B FIFO 有空位 ---- */
        if (!VS1003_WaitDREQ(500)) {
            a_error = AUDIO_ERR_VS1003;
            break;
        }

        /* ---- 从 SD 读一块 ---- */
        uint32_t bytes = sd_sync(SD_CMD_READ, NULL, audio_buf, 2048, 0);
        if (bytes == 0) break;

        /* ---- 发送到 VS1003B ---- */
        VS1003_SendData(audio_buf, (uint16_t)bytes);
        a_sent += bytes;
        if (a_total > 0) {
            uint8_t p = (uint8_t)((a_sent * 100ULL) / a_total);
            a_progress = (p > 100) ? 100 : p;
        }
    }

    a_playing  = 0;
    a_progress = 100;
    send_zeros();
    sd_sync(SD_CMD_CLOSE, NULL, NULL, 0, 0);
    return PLAY_DONE;
}

void StartAudioTask(void *argument)
{
    /* SD 任务优先级更高, 等它挂载完成 */
    osDelay(2000);

    uint8_t cnt = SD_GetTrackCount();
    if (cnt == 0) {
        a_error = AUDIO_ERR_NO_FILES;
        for (;;) osDelay(1000);
    }

    /* 初始化 VS1003B */
    a_volume = 0xCC;
    if (!VS1003_Init()) {
        a_error = AUDIO_ERR_VS1003;
        for (;;) osDelay(1000);
    }
    VS1003_SetPlayMode();
    a_error = AUDIO_ERR_NONE;

    /* 循环播放 */
    uint8_t trk = 0;
    for (;;) {
        const char* nm = SD_GetTrackName(trk);
        if (!nm) { trk = 0; cnt = SD_GetTrackCount(); continue; }

        char path[128];
        strcpy(path, "0:/");
        strcat(path, nm);

        a_track = (int8_t)trk;
        osSemaphoreRelease(lcdWakeSem);
        osDelay(80);

        uint8_t result = play_one(path);

        if (result == PLAY_DELETED) {
            cnt = SD_GetTrackCount();
            if (cnt == 0) {
                a_error = AUDIO_ERR_NO_FILES;
                for (;;) osDelay(1000);
            }
            if (trk >= cnt) trk = 0;
        } else {
            trk++;
            if (trk >= cnt) trk = 0;
            osDelay(50);
        }
    }
}
