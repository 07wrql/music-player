/**
 * @file    ymodem.h
 * @brief   YModem 协议接收器 — 用于通过 UART 接收固件升级文件
 *
 * 协议栈:
 *   PC (SecureCRT/Xshell/ymodem-send) ──UART──→ STM32
 *
 * 使用方式:
 *   1. 调用 YModem_Init() 配置 UART 句柄
 *   2. 调用 YModem_Receive() 阻塞等待并接收固件
 *   3. 检查返回值，成功则设置更新标志并复位
 */

#ifndef __YMODEM_H
#define __YMODEM_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_uart.h"

/* YModem 协议常量 */
#define YMODEM_PACKET_SIZE      1024    /* 数据包大小 */
#define YMODEM_PACKET_HEADER    128     /* 文件名包头大小 (packet 0) */
#define YMODEM_SOH              0x01    /* 128-byte packet marker */
#define YMODEM_STX              0x02    /* 1024-byte packet marker */
#define YMODEM_EOT              0x04    /* End of Transmission */
#define YMODEM_ACK              0x06    /* Acknowledge */
#define YMODEM_NAK              0x15    /* Negative Acknowledge */
#define YMODEM_CAN              0x18    /* Cancel */
#define YMODEM_C                0x43    /* 'C' — request CRC mode */

/* 超时定义 (ms) */
#define YMODEM_TIMEOUT_START    60000   /* 等待发送方开始: 60s */
#define YMODEM_TIMEOUT_PACKET   5000    /* 等待单个数据包: 5s */
#define YMODEM_MAX_ERRORS       10      /* 最大连续错误数 */

/**
 * @brief  初始化 YModem (绑定 UART)
 * @param  huart  指向 HAL UART 句柄
 */
void YModem_Init(UART_HandleTypeDef *huart);

/**
 * @brief  阻塞等待并接收固件文件, 写入到目标 Flash 地址
 * @param  flash_dest  固件写入目标地址 (APP2_BASE)
 * @param  max_size    最大允许的固件大小 (字节)
 * @retval >0 = 固件大小 (成功)
 *         -1 = 用户取消或超时
 *         -2 = Flash 写入错误
 *         -3 = CRC 校验错误
 */
int YModem_Receive(uint32_t flash_dest, uint32_t max_size);

#endif /* __YMODEM_H */
