/**
 * @file    ymodem.c
 * @brief   YModem 协议接收器实现
 *
 * YModem 是基于 XModem-1K 的改进协议:
 *   - Packet 0: 文件名 + 文件大小 (128 bytes, 总是 SOH)
 *   - Packet 1..N: 数据 (1024 bytes, STX)
 *   - 最后包不足 1024 用 0x1A (CPMEOF) 填充
 *   - EOT 发两次 (先 NAK 再 ACK)
 *   - 最后发一个空的 packet 0 表示批次结束
 *
 * CRC16-CCITT 校验, polynomial = 0x1021
 */

#include "ymodem.h"
#include "flash_if.h"
#include "iap_config.h"
#include <string.h>

static UART_HandleTypeDef *ym_huart = NULL;

/* ================================================================
   UART 底层读写
   ================================================================ */

static int uart_rx_byte(uint8_t *ch, uint32_t timeout_ms)
{
    if (HAL_UART_Receive(ym_huart, ch, 1, timeout_ms) != HAL_OK)
        return -1;
    return 0;
}

static void uart_tx_byte(uint8_t ch)
{
    HAL_UART_Transmit(ym_huart, &ch, 1, 100);
}

/**
 * @brief  尝试读一字节, 失败则发送 CAN 并退出
 */
#define RX_OR_CANCEL(ch, timeout) \
    do { if (uart_rx_byte(&(ch), (timeout)) != 0) goto cancel; } while(0)

/* ================================================================
   CRC16-CCITT
   ================================================================ */
static uint16_t ymodem_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)buf[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

/* ================================================================
   Flash 写入缓冲区 (按页缓冲, 减少 Flash 编程次数)
   ================================================================ */
#define WRITE_BUF_WORDS  256    /* 1024 字节 = 256 个 word */

static uint32_t write_buf[WRITE_BUF_WORDS];
static uint32_t write_buf_idx = 0;
static uint32_t write_addr     = 0;

static int write_buf_flush(void)
{
    if (write_buf_idx == 0) return 0;
    int r = flash_write_buf(write_addr, write_buf, write_buf_idx);
    write_addr     += write_buf_idx * 4;
    write_buf_idx   = 0;
    return r;
}

static int write_buf_push(uint32_t word)
{
    write_buf[write_buf_idx++] = word;
    if (write_buf_idx >= WRITE_BUF_WORDS)
        return write_buf_flush();
    return 0;
}

/* ================================================================
   YModem 主接收函数
   ================================================================ */

void YModem_Init(UART_HandleTypeDef *huart)
{
    ym_huart = huart;
}

int YModem_Receive(uint32_t flash_dest, uint32_t max_size)
{
    uint8_t  ch;
    uint16_t crc, rx_crc;
    uint32_t fw_size = 0, fw_received = 0;
    uint16_t pkt_len;
    uint8_t  pkt_type;
    uint8_t  blk_num, blk_cmp;
    int      errors = 0;
    int      phase  = 0;  /* 0=wait header, 1=data, 2=EOT */

    if (ym_huart == NULL) return -1;
    if (flash_dest != APP2_BASE + FW_HEADER_SIZE) return -1;

    /* ---- 先擦除目标 Flash 区域 (APP2), 再发起传输 ----
       擦除耗时数秒且期间 CPU 停摆; PC 端等待初始 'C' 超时为 30s,
       若擦除放到传输中途会造成丢包, 导致升级失败 */
    {
        int r = flash_erase_sectors(APP2_SECTOR_START, APP2_SECTOR_COUNT);
        if (r != 0) return -2;
    }

    /* ---- 第一阶段: 等待发送方 ---- */
    uart_tx_byte(YMODEM_C);  /* 发送 'C' 发起 CRC 模式传输 */

    /* ---- 第二阶段: 接收文件头 (packet 0) ---- */
    RX_OR_CANCEL(pkt_type, YMODEM_TIMEOUT_START);

    if (pkt_type == YMODEM_CAN) return -1;  /* 对方取消 */

    if (pkt_type == YMODEM_EOT)
    {
        /* 空目录 → 没有文件, 发 ACK 后退出 */
        uart_tx_byte(YMODEM_ACK);
        return -1;
    }

    if (pkt_type != YMODEM_SOH) goto cancel;

    /* 读 block number (0) 及其反码 */
    RX_OR_CANCEL(blk_num, YMODEM_TIMEOUT_PACKET);
    RX_OR_CANCEL(blk_cmp, YMODEM_TIMEOUT_PACKET);
    if (blk_num != 0 || (blk_num + blk_cmp) != 0xFF) goto cancel;

    /* 读 128 字节文件头 */
    {
        uint8_t header[128];
        for (int i = 0; i < 128; i++)
            { RX_OR_CANCEL(header[i], YMODEM_TIMEOUT_PACKET); }

        /* 读 CRC */
        RX_OR_CANCEL(ch, YMODEM_TIMEOUT_PACKET); rx_crc  = (uint16_t)ch << 8;
        RX_OR_CANCEL(ch, YMODEM_TIMEOUT_PACKET); rx_crc |= ch;

        /* 校验 CRC */
        if (rx_crc != ymodem_crc16(header, 128)) goto cancel;

        /* 解析文件大小: 格式 "filename\x00size\n..." */
        char *p = (char*)header;
        while (*p != '\0' && p < (char*)header + 128) p++;  /* 跳过文件名 */
        p++;  /* 跳过 \0 */

        /* 解析十进制 size */
        fw_size = 0;
        while (*p >= '0' && *p <= '9' && p < (char*)header + 128)
        {
            fw_size = fw_size * 10 + (uint32_t)(*p - '0');
            p++;
        }

        if (fw_size == 0 || fw_size > max_size)
        {
            uart_tx_byte(YMODEM_CAN);
            uart_tx_byte(YMODEM_CAN);
            return -1;
        }
    }

    /* 文件头接收成功 → ACK + 'C' 开始要数据 */
    uart_tx_byte(YMODEM_ACK);
    uart_tx_byte(YMODEM_C);

    /* 初始化写入缓冲 */
    write_buf_idx = 0;
    write_addr    = flash_dest;

    /* ---- 第三阶段: 接收数据包 ---- */
    phase = 1;
    for (;;)
    {
        ch = 0;
        if (uart_rx_byte(&ch, YMODEM_TIMEOUT_PACKET) != 0)
        {
            errors++;
            if (errors > YMODEM_MAX_ERRORS) goto cancel;
            uart_tx_byte(YMODEM_NAK);
            continue;
        }

        if (ch == YMODEM_EOT)
        {
            /* 第一次 EOT → NAK, 等第二次 EOT → ACK */
            uart_tx_byte(YMODEM_NAK);

            RX_OR_CANCEL(ch, YMODEM_TIMEOUT_PACKET);
            if (ch != YMODEM_EOT) goto cancel;

            uart_tx_byte(YMODEM_ACK);
            phase = 2;
            break;
        }

        if (ch == YMODEM_CAN)
        {
            /* 发送方取消 */
            goto cancel;
        }

        if (ch == YMODEM_SOH)
            pkt_len = 128;
        else if (ch == YMODEM_STX)
            pkt_len = YMODEM_PACKET_SIZE;
        else
        {
            errors++;
            uart_tx_byte(YMODEM_NAK);
            continue;
        }

        /* 读 block number 和 反码 */
        RX_OR_CANCEL(blk_num, YMODEM_TIMEOUT_PACKET);
        RX_OR_CANCEL(blk_cmp, YMODEM_TIMEOUT_PACKET);

        if ((blk_num + blk_cmp) != 0xFF)
        {
            errors++;
            uart_tx_byte(YMODEM_NAK);
            continue;
        }

        /* 读数据 + CRC */
        {
            uint8_t  data[YMODEM_PACKET_SIZE];
            uint16_t actual = (fw_received + pkt_len <= fw_size)
                              ? pkt_len : (fw_size - fw_received);

            for (uint16_t i = 0; i < pkt_len; i++)
                { RX_OR_CANCEL(data[i], YMODEM_TIMEOUT_PACKET); }

            RX_OR_CANCEL(ch, YMODEM_TIMEOUT_PACKET); rx_crc  = (uint16_t)ch << 8;
            RX_OR_CANCEL(ch, YMODEM_TIMEOUT_PACKET); rx_crc |= ch;

            /* CRC 校验 */
            if (rx_crc != ymodem_crc16(data, pkt_len))
            {
                errors++;
                uart_tx_byte(YMODEM_NAK);
                continue;
            }

            /* 写入 Flash (按字缓冲) */
            if (actual > 0)
            {
                for (uint16_t i = 0; i < actual; i += 4)
                {
                    uint32_t word = *((uint32_t*)(data + i));
                    if (write_buf_push(word) != 0)
                    {
                        write_buf_flush();
                        return -2;
                    }
                }
                fw_received += actual;
            }

            errors = 0;
            uart_tx_byte(YMODEM_ACK);

            if (fw_received >= fw_size)
                break;
        }
    }

    /* ---- 第四阶段: 结束传输 ---- */
    /* 刷新写入缓冲 */
    if (write_buf_flush() != 0) return -2;

    /* 等待最后的空文件头 (批次结束) */
    {
        uint8_t dummy[133];  /* SOH + 00 + FF + 128 bytes + CRC(2) */
        for (int i = 0; i < 133; i++)
        {
            if (uart_rx_byte(&ch, 1000) != 0) break;
            dummy[i] = ch;
        }
        uart_tx_byte(YMODEM_ACK);
    }

    return (int)fw_received;

cancel:
    uart_tx_byte(YMODEM_CAN);
    uart_tx_byte(YMODEM_CAN);
    return -1;
}
