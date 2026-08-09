/**
 * @file    bootloader_uart.h
 * @brief   UART IAP 协议 — 参照参考项目
 */

#ifndef __BOOTLOADER_UART_H
#define __BOOTLOADER_UART_H

#include "stm32f4xx_hal.h"

/* IAP 命令 */
#define IAP_CMD_ERASE          0xA0
#define IAP_CMD_WRITE          0xA1
#define IAP_CMD_READ           0xA2
#define IAP_CMD_JUMP           0xA3
#define IAP_CMD_GET_INFO       0xA4
#define IAP_CMD_START_UPDATE   0xA5
#define IAP_CMD_END_UPDATE     0xA6
#define IAP_CMD_VERIFY         0xA7

/* 数据包 */
typedef struct {
    uint8_t  cmd;
    uint16_t len;
    uint32_t addr;
    uint8_t  data[1024];
} IAP_Packet_TypeDef;

void Bootloader_UART_Init(UART_HandleTypeDef *huart);
void Bootloader_UART_Process(void);
void Bootloader_UART_SendString(const char *s);

#endif /* __BOOTLOADER_UART_H */
