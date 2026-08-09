/**
 * @file    sd_task.c
 * @brief   SD卡读写任务 — 通过信号量+共享请求同步
 */
#include "sd_task.h"
#include "main.h"
#include "fatfs.h"
#include "iap_config.h"
#include "flash_if.h"
#include <string.h>

static FATFS   sd_fs;
static FIL     sd_file;       /* 读音频文件 */
static FIL     sd_file_wr;    /* 写文件 (PC→SD) */
static FIL     sd_font_file;  /* 中文字库文件 (FONT.DZK) */
static volatile uint8_t sd_mounted = 0;
static volatile uint8_t sd_font_ok   = 0;  /* 字库是否可用 */

#define MAX_PL 64
#define MAX_FN 64
static char    playlist[MAX_PL][MAX_FN];
static volatile uint8_t pl_count = 0;

osSemaphoreId_t sdReqSem  = NULL;
osSemaphoreId_t sdDoneSem = NULL;
osMutexId_t     sdMutex   = NULL;  /* 串行化 sd_sync 多任务调用 */

/* 共享请求（调用者填写，SD_Task处理后设置result） */
SD_Request sd_req;

static uint8_t scan_dir(void)
{
    DIR dir; FILINFO info;
    pl_count = 0;
    memset(playlist, 0, sizeof(playlist));
    if (f_opendir(&dir, "0:/") != FR_OK) return 0;
    while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
        if (pl_count >= MAX_PL) break;
        if (info.fattrib & (AM_DIR|AM_HID|AM_SYS)) continue;
        int len = strlen(info.fname);
        if (len < 4) continue;
        const char *e4 = info.fname + len - 4;
        int is_audio = 0;
        if (e4[0] == '.') {
            if ((e4[1]=='M'||e4[1]=='m') && (e4[2]=='P'||e4[2]=='p') && (e4[3]=='3'))
                is_audio = 1;
            if ((e4[1]=='W'||e4[1]=='w') && (e4[2]=='A'||e4[2]=='a') && (e4[3]=='V'||e4[3]=='v'))
                is_audio = 1;
        }
        if (is_audio) {
            strncpy(playlist[pl_count], info.fname, MAX_FN-1);
            playlist[pl_count][MAX_FN-1] = '\0';
            pl_count++;
        }
    }
    f_closedir(&dir);
    return pl_count;
}

void StartSDTask(void *argument)
{
    /* 启动时挂载 SD */
    FRESULT fr = f_mount(&sd_fs, "0:", 1);
    sd_mounted = (fr == FR_OK);
    if (sd_mounted) {
        scan_dir();

        /* 打开中文字库文件 (失败不阻塞, 只影响中文显示) */
        if (f_open(&sd_font_file, "0:/FONT.DZK", FA_READ) == FR_OK) {
            sd_font_ok = 1;
        }

        /* SD 卡升级: 检查根目录是否有 UPDATE.bin */
        FIL upd_file;
        if (f_open(&upd_file, "0:/UPDATE.bin", FA_READ) == FR_OK) {
            uint32_t fw_size = (uint32_t)f_size(&upd_file);
            if (fw_size > 0 && fw_size <= (APP2_SIZE - FW_HEADER_SIZE)) {
                /* 擦除 APP2 */
                flash_erase_sectors(APP2_SECTOR_START, APP2_SECTOR_COUNT);

                /* 写固件头 */
                fw_header_t hdr;
                hdr.magic      = FW_HEADER_MAGIC;
                hdr.fw_size    = fw_size;
                hdr.fw_version = 0x00010001;
                hdr.fw_crc     = 0;
                flash_write_buf(APP2_BASE, (uint32_t*)&hdr, FW_HEADER_SIZE / 4);

                /* 从 SD 读 .bin 写入 APP2 (4KB 块) */
                static uint8_t buf[4096];  /* static: 避免 4KB 栈溢出 */
                UINT br;
                uint32_t addr = APP2_BASE + FW_HEADER_SIZE;
                while (f_read(&upd_file, buf, sizeof(buf), &br) == FR_OK && br > 0) {
                    flash_write_buf(addr, (uint32_t*)buf, (br + 3) / 4);
                    addr += br;
                }
                f_close(&upd_file);

                /* 擦除参数区最后一个扇区(S11), 写入更新标志 */
                flash_erase_sectors(PARAM_SECTOR_START + PARAM_SECTOR_COUNT - 1, 1);
                uint32_t flag = UPDATE_FLAG_MAGIC;
                flash_write_word(UPDATE_FLAG_ADDR, flag);

                /* 删除 UPDATE.bin, 防止反复升级 */
                f_unlink("0:/UPDATE.bin");

                /* 复位, 让 main.c 启动代码复制 APP2→APP1 */
                NVIC_SystemReset();
                while(1);
            } else {
                f_close(&upd_file);
            }
        }
    }

    for (;;) {
        /* 等待 Audio_Task 发请求 */
        osSemaphoreAcquire(sdReqSem, osWaitForever);

        switch (sd_req.cmd) {
        case SD_CMD_MOUNT:
            fr = f_mount(&sd_fs, "0:", 1);
            sd_mounted = (fr == FR_OK);
            sd_req.result = (uint32_t)fr;
            if (sd_mounted) scan_dir();
            break;
        case SD_CMD_OPEN:
            sd_req.result = sd_mounted ? (uint32_t)f_open(&sd_file, sd_req.path, FA_READ) : FR_DISK_ERR;
            break;
        case SD_CMD_READ: {
            UINT br = 0;
            FRESULT r = FR_INVALID_PARAMETER;
            if (sd_req.buf && sd_req.size > 0)
                r = f_read(&sd_file, sd_req.buf, sd_req.size, &br);
            sd_req.result = (r == FR_OK) ? br : 0;
            break;
        }
        case SD_CMD_CLOSE:
            sd_req.result = (uint32_t)f_close(&sd_file);
            break;
        case SD_CMD_GET_SIZE:
            sd_req.result = (uint32_t)f_size(&sd_file);
            break;
        case SD_CMD_LSEEK:
            sd_req.result = (uint32_t)f_lseek(&sd_file, sd_req.offset);
            break;
        case SD_CMD_DELETE:
            sd_req.result = sd_mounted ? (uint32_t)f_unlink(sd_req.path) : FR_DISK_ERR;
            break;
        case SD_CMD_WRITE_OPEN:
            sd_req.result = sd_mounted
                ? (uint32_t)f_open(&sd_file_wr, sd_req.path,
                                   FA_CREATE_ALWAYS | FA_WRITE)
                : FR_DISK_ERR;
            break;
        case SD_CMD_WRITE: {
            UINT bw = 0;
            FRESULT r = FR_INVALID_PARAMETER;
            if (sd_req.buf && sd_req.size > 0)
                r = f_write(&sd_file_wr, sd_req.buf, sd_req.size, &bw);
            sd_req.result = (r == FR_OK) ? bw : 0;
            break;
        }
        case SD_CMD_WRITE_CLOSE:
            sd_req.result = (uint32_t)f_close(&sd_file_wr);
            break;
        case SD_CMD_FONT_READ: {
            UINT br = 0;
            if (sd_font_ok) {
                f_lseek(&sd_font_file, sd_req.offset);
                FRESULT r = f_read(&sd_font_file, sd_req.buf, sd_req.size, &br);
                sd_req.result = (r == FR_OK) ? br : 0;
            } else {
                sd_req.result = 0;
            }
            break;
        }
        default:
            sd_req.result = 0xFFFFFFFF;
            break;
        }

        /* 通知调用者：处理完成 */
        osSemaphoreRelease(sdDoneSem);
    }
}

uint8_t     SD_GetTrackCount(void)       { return pl_count; }
const char* SD_GetTrackName(uint8_t idx) { return (idx < pl_count) ? playlist[idx] : NULL; }
uint8_t     SD_IsMounted(void)           { return sd_mounted; }
uint8_t     SD_FontOK(void)              { return sd_font_ok; }

/* ---- 公开的同步请求函数 (也供 lcd_task 读字库) ---- */
uint32_t sd_sync(SD_CmdType cmd, const char* path,
                 uint8_t* buf, uint32_t size, uint32_t offset)
{
    uint32_t result;

    if (osMutexAcquire(sdMutex, 200) != osOK) {  /* 200ms */
        return 0;
    }

    sd_req.cmd    = cmd;
    sd_req.buf    = buf;
    sd_req.size   = size;
    sd_req.offset = offset;
    if (path) {
        strncpy(sd_req.path, path, sizeof(sd_req.path) - 1);
        sd_req.path[sizeof(sd_req.path) - 1] = '\0';
    } else {
        sd_req.path[0] = '\0';
    }

    osSemaphoreRelease(sdReqSem);
    if (osSemaphoreAcquire(sdDoneSem, 500) != osOK) {  /* 500ms */
        osMutexRelease(sdMutex);
        return 0;
    }
    result = sd_req.result;

    osMutexRelease(sdMutex);
    return result;
}
