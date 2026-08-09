/**
 * @file    uart_recv.c
 * @brief   UART 命令接收 — FreeRTOS 运行时处理
 *
 * 支持命令:
 *   #UPDATE#  — 进入 YModem 固件接收模式 (写入 APP2 Flash)
 *   #SEND#    — 通过 YModem 接收文件写入 SD 卡
 *
 * 由 defaultTask 周期性调用 UartRecv_Poll()
 */

#include "uart_recv.h"
#include "sd_task.h"
#include "main.h"
#include "usart.h"
#include "iap_config.h"
#include "flash_if.h"
#include <string.h>
#include <stdio.h>

extern SD_Request sd_req;
extern osSemaphoreId_t sdReqSem;
extern osSemaphoreId_t sdDoneSem;

/* ---- UART 环形缓冲 (手动 RXNE 中断, 不经过 HAL 锁) ---- */
#define RING_SIZE 256
static volatile uint8_t uart_ring[RING_SIZE];
static volatile int      uart_ring_w = 0;
static volatile int      uart_ring_r = 0;

void UART_RX_ISR(void)
{
    /* 排空接收寄存器: 一次中断可能已有多字节到达 (115200bps 下 ~87us/字节) */
    while (USART1->SR & USART_SR_RXNE) {
        uint8_t ch = (uint8_t)(USART1->DR & 0xFF);
        int next = (uart_ring_w + 1) % RING_SIZE;
        if (next != uart_ring_r) {
            uart_ring[uart_ring_w] = ch;
            uart_ring_w = next;
        }
    }
}

static int ring_get(uint8_t *ch)
{
    if (uart_ring_r == uart_ring_w) return -1;
    *ch = uart_ring[uart_ring_r];
    uart_ring_r = (uart_ring_r + 1) % RING_SIZE;
    return 0;
}

static int ring_available(void)
{
    return (uart_ring_w - uart_ring_r + RING_SIZE) % RING_SIZE;
}

/* ---- 启动 RX 中断 ---- */
static uint8_t rx_enabled = 0;
static void uart_rx_start(void)
{
    if (rx_enabled) return;
    USART1->CR1 |= USART_CR1_RXNEIE;
    rx_enabled = 1;
}

/* ---- UART 底层读写 ---- */
static int uart_rx_byte(uint8_t *ch, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        if (ring_get(ch) == 0) return 0;
    }
    return -1;
}

static void uart_tx_byte(uint8_t ch)
{
    HAL_UART_Transmit(&huart1, &ch, 1, 100);
}

static void uart_tx_str(const char *s)
{
    while (*s) uart_tx_byte((uint8_t)*s++);
}

/* ---- YModem CRC16 ---- */
static uint16_t crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

/* ---- SD 同步请求 (通过消息队列) ---- */
static uint32_t sd_sync(SD_CmdType cmd, const char *path,
                        uint8_t *buf, uint32_t size, uint32_t offset)
{
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
    osSemaphoreAcquire(sdDoneSem, osWaitForever);
    return sd_req.result;
}

/* ================================================================
   YModem 接收 → SD 卡文件
   ================================================================ */
#define RB_SIZE 1024
static uint8_t rx_buf[RB_SIZE];

static void recv_file_to_sd(void)
{
    uart_tx_byte(0x43);  /* 'C' */

    /* Packet 0: 文件名 + 大小 */
    uint8_t ch, blk, blkc;
    uint16_t crc;
    uint32_t file_size = 0, file_got = 0;
    char filename[64] = {0}, path[128];

    if (uart_rx_byte(&ch, 30000) || ch == 0x18 || ch != 0x01) return;
    uart_rx_byte(&blk, 5000); uart_rx_byte(&blkc, 5000);
    if (blk != 0 || (blk + blkc) != 0xFF) return;

    uint8_t header[128];
    for (int i = 0; i < 128; i++) uart_rx_byte(&header[i], 5000);
    uart_rx_byte(&ch, 5000); crc  = (uint16_t)ch << 8;
    uart_rx_byte(&ch, 5000); crc |= ch;
    if (crc != crc16(header, 128)) { uart_tx_byte(0x15); return; }

    /* 解析文件名 */
    int i;
    for (i = 0; i < 63 && header[i] && header[i] != ' '; i++)
        filename[i] = (char)header[i];
    filename[i] = 0;

    /* 解析文件大小 */
    char *p = (char *)header;
    while (*p && p < (char *)header + 128) p++; p++;
    file_size = 0;
    while (*p >= '0' && *p <= '9') {
        file_size = file_size * 10 + (uint32_t)(*p - '0');
        p++;
    }

    if (!filename[0] || !file_size) {
        uart_tx_byte(0x18); uart_tx_byte(0x18);
        return;
    }

    sprintf(path, "0:/%s", filename);
    uart_tx_byte(0x06); uart_tx_byte(0x43);

    if (sd_sync(SD_CMD_WRITE_OPEN, path, NULL, 0, 0) != FR_OK) {
        uart_tx_byte(0x18); uart_tx_byte(0x18);
        return;
    }

    /* 接收数据包 */
    int errors = 0;
    for (;;) {
        if (uart_rx_byte(&ch, 5000)) {
            errors++;
            if (errors > 5) break;
            uart_tx_byte(0x15);
            continue;
        }
        if (ch == 0x04) {  /* EOT */
            uart_tx_byte(0x15);
            uart_rx_byte(&ch, 5000);
            if (ch == 0x04) uart_tx_byte(0x06);
            break;
        }
        if (ch == 0x18) break;  /* CAN */

        uint16_t pl = (ch == 0x02) ? 1024 : ((ch == 0x01) ? 128 : 0);
        if (!pl) { errors++; uart_tx_byte(0x15); continue; }

        uart_rx_byte(&blk, 5000); uart_rx_byte(&blkc, 5000);
        if ((blk + blkc) != 0xFF) { errors++; uart_tx_byte(0x15); continue; }

        for (uint16_t j = 0; j < pl; j++) uart_rx_byte(&rx_buf[j], 5000);
        uart_rx_byte(&ch, 5000); crc  = (uint16_t)ch << 8;
        uart_rx_byte(&ch, 5000); crc |= ch;

        if (crc != crc16(rx_buf, pl)) { errors++; uart_tx_byte(0x15); continue; }

        uint16_t actual = (file_got + pl <= file_size) ? pl : (uint16_t)(file_size - file_got);
        if (actual && sd_sync(SD_CMD_WRITE, NULL, rx_buf, actual, 0) != actual) {
            uart_tx_byte(0x18); break;
        }
        file_got += actual;
        errors = 0;
        uart_tx_byte(0x06);
        if (file_got >= file_size) break;
    }

    sd_sync(SD_CMD_WRITE_CLOSE, NULL, NULL, 0, 0);
    sd_sync(SD_CMD_MOUNT, NULL, NULL, 0, 0);
    uart_tx_str("\r\nDONE\r\n");
}

/* ================================================================
   YModem 接收 → APP2 Flash (固件升级)
   与 main.c 中的 IAP 模式逻辑相同, 但在 FreeRTOS 环境下运行
   ================================================================ */
static void recv_firmware_to_flash(void)
{
    /* 先擦除 APP2（耗时数秒且期间 CPU 停摆）:
       PC 端等待 'C' 超时较长, 放在传输中途会丢包, 放在最前面最稳 */
    if (flash_erase_sectors(APP2_SECTOR_START, APP2_SECTOR_COUNT) != 0) {
        uart_tx_str("ERROR: flash erase fail\r\n");
        return;
    }

    uart_tx_str("READY\r\n");  /* 通知 PC 端可以发送 */
    uart_tx_byte(0x43);        /* 'C' */

    uint8_t ch, blk, blkc;
    uint16_t crc;
    uint32_t fw_size = 0, fw_got = 0;

    /* Packet 0: 文件名 + 大小 */
    if (uart_rx_byte(&ch, 60000) || ch == 0x18 || ch != 0x01) {
        uart_tx_str("ERROR: no header\r\n");
        return;
    }
    uart_rx_byte(&blk, 5000); uart_rx_byte(&blkc, 5000);
    if (blk != 0 || (blk + blkc) != 0xFF) {
        uart_tx_str("ERROR: bad header\r\n");
        return;
    }

    uint8_t header[128];
    for (int i = 0; i < 128; i++) uart_rx_byte(&header[i], 5000);
    uart_rx_byte(&ch, 5000); crc  = (uint16_t)ch << 8;
    uart_rx_byte(&ch, 5000); crc |= ch;
    if (crc != crc16(header, 128)) {
        uart_tx_byte(0x15);
        uart_tx_str("ERROR: CRC fail\r\n");
        return;
    }

    /* 解析文件大小 */
    char *p = (char *)header;
    while (*p && p < (char *)header + 128) p++; p++;
    fw_size = 0;
    while (*p >= '0' && *p <= '9') {
        fw_size = fw_size * 10 + (uint32_t)(*p - '0');
        p++;
    }

    if (fw_size == 0 || fw_size > APP_FW_MAX_SIZE) {
        uart_tx_byte(0x18); uart_tx_byte(0x18);
        uart_tx_str("ERROR: bad size\r\n");
        return;
    }

    uart_tx_byte(0x06); uart_tx_byte(0x43);

    /* 接收数据并写入 Flash */
    uint32_t write_addr = APP2_BASE + FW_HEADER_SIZE;
    int errors = 0;

    for (;;) {
        if (uart_rx_byte(&ch, 5000)) {
            errors++;
            if (errors > 10) break;
            uart_tx_byte(0x15);
            continue;
        }
        if (ch == 0x04) {  /* EOT */
            uart_tx_byte(0x15);
            uart_rx_byte(&ch, 5000);
            if (ch == 0x04) uart_tx_byte(0x06);
            break;
        }
        if (ch == 0x18) break;

        uint16_t pl = (ch == 0x02) ? 1024 : ((ch == 0x01) ? 128 : 0);
        if (!pl) { errors++; uart_tx_byte(0x15); continue; }

        uart_rx_byte(&blk, 5000); uart_rx_byte(&blkc, 5000);
        if ((blk + blkc) != 0xFF) { errors++; uart_tx_byte(0x15); continue; }

        for (uint16_t j = 0; j < pl; j++) uart_rx_byte(&rx_buf[j], 5000);
        uart_rx_byte(&ch, 5000); crc  = (uint16_t)ch << 8;
        uart_rx_byte(&ch, 5000); crc |= ch;

        if (crc != crc16(rx_buf, pl)) { errors++; uart_tx_byte(0x15); continue; }

        uint16_t actual = (fw_got + pl <= fw_size) ? pl : (uint16_t)(fw_size - fw_got);
        if (actual > 0) {
            uint32_t words = (actual + 3) / 4;
            if (flash_write_buf(write_addr, (uint32_t *)rx_buf, words) != 0) {
                uart_tx_str("ERROR: flash write fail\r\n");
                return;
            }
            write_addr += actual;
            fw_got += actual;
        }

        errors = 0;
        uart_tx_byte(0x06);
        if (fw_got >= fw_size) break;
    }

    /* 写入固件头 */
    fw_header_t hdr;
    hdr.magic      = FW_HEADER_MAGIC;
    hdr.fw_size    = fw_got;
    hdr.fw_version = 0x00010001;  /* v1.1 */
    hdr.fw_crc     = 0;           /* 后续可扩展 */

    flash_write_buf(APP2_BASE, (uint32_t *)&hdr, FW_HEADER_SIZE / 4);

    /* 设置更新标志 */
    uint32_t flag = UPDATE_FLAG_MAGIC;
    flash_erase_sectors(PARAM_SECTOR_START + PARAM_SECTOR_COUNT - 1, 1);
    flash_write_word(UPDATE_FLAG_ADDR, flag);

    uart_tx_str("\r\nUPDATE OK. Rebooting...\r\n");

    /* 延时等待串口发送完成, 然后软复位 */
    HAL_Delay(500);
    NVIC_SystemReset();
    while (1);
}

/* ================================================================
   命令缓冲区与解析
   ================================================================ */
#define CMD_BUF_SIZE 32
static char cmd_buf[CMD_BUF_SIZE];
static int  cmd_idx = 0;

static void process_cmd(const char *cmd)
{
    if (strcmp(cmd, "#UPDATE#") == 0) {
        uart_tx_str("IAP Firmware Update Mode\r\n");
        uart_tx_str("Send .bin file via YModem...\r\n");
        recv_firmware_to_flash();
    }
    else if (strcmp(cmd, "#SEND#") == 0) {
        uart_tx_str("File Receive Mode (SD Card)\r\n");
        recv_file_to_sd();
    }
    else if (strcmp(cmd, "#INFO#") == 0) {
        char info[64];
        sprintf(info, "APP1 v1.0 @ 0x%08X, FreeRTOS\r\n", (unsigned int)APP1_BASE);
        uart_tx_str(info);
    }
    else if (cmd[0] != '\0') {
        uart_tx_str("Unknown: ");
        uart_tx_str(cmd);
        uart_tx_str("\r\nCommands: #UPDATE# #SEND# #INFO#\r\n");
    }
}

/* ================================================================
   UartRecv_Poll — 由 defaultTask 每 50ms 调用一次
   ================================================================ */
void UartRecv_Poll(void)
{
    uart_rx_start();

    uint8_t ch;
    while (ring_get(&ch) == 0)
    {
        if (ch == '\r' || ch == '\n') {
            /* 行结束 → 处理命令 */
            if (cmd_idx > 0) {
                cmd_buf[cmd_idx] = '\0';
                process_cmd(cmd_buf);
                cmd_idx = 0;
            }
        }
        else if (cmd_idx < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_idx++] = (char)ch;
        }
    }
}
