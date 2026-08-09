#ifndef __UART_RECV_H
#define __UART_RECV_H

#include "cmsis_os.h"

/* 由 defaultTask 周期调用, 监听 UART 指令并处理文件接收 */
void UartRecv_Poll(void);

/* USART1 RXNE 中断处理: 读入环形缓冲 (由 stm32f4xx_it.c 调用) */
void UART_RX_ISR(void);

#endif
