#ifndef __SD_TASK_H
#define __SD_TASK_H

#include "cmsis_os.h"
#include <stdint.h>
#include "ff.h"

/* 请求结构体 */
typedef enum {
    SD_CMD_OPEN, SD_CMD_READ, SD_CMD_CLOSE,
    SD_CMD_MOUNT, SD_CMD_GET_SIZE, SD_CMD_LSEEK,
    SD_CMD_DELETE,        /* 删除文件: path, result=FRESULT */
    SD_CMD_WRITE_OPEN,    /* 创建文件用于写入: path, result=FRESULT */
    SD_CMD_WRITE,         /* 写数据到当前文件: buf/size, result=写入字节数 */
    SD_CMD_WRITE_CLOSE,   /* 关闭写入的文件: result=FRESULT */
    SD_CMD_FONT_READ,     /* 读取字库: offset=字模偏移, buf[32], result=读取字节数 */
} SD_CmdType;

typedef struct {
    SD_CmdType cmd;
    char       path[128];
    uint8_t*   buf;
    uint32_t   size;
    uint32_t   offset;
    uint32_t   result;
} SD_Request;

/* 信号量句柄 */
extern osSemaphoreId_t sdReqSem;
extern osSemaphoreId_t sdDoneSem;
extern osMutexId_t     sdMutex;   /* 串行化 sd_sync */

/* 共享请求对象 (调用者填写, SD_Task 处理后设置 result) */
extern SD_Request sd_req;

/* 同步请求: 阻塞发送并等待结果 */
uint32_t sd_sync(SD_CmdType cmd, const char* path,
                 uint8_t* buf, uint32_t size, uint32_t offset);

void StartSDTask(void *argument);

uint8_t     SD_GetTrackCount(void);
const char* SD_GetTrackName(uint8_t idx);
uint8_t     SD_IsMounted(void);
uint8_t     SD_FontOK(void);       /* 字库文件 FONT.DZK 是否可用 */

#endif
