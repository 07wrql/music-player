/**
 * @file    bootloader_uart.c
 * @brief   UART IAP 二进制协议 — 完全参照参考项目
 */

#include "bootloader_uart.h"
#include "bootloader.h"
#include <string.h>

#define PACKET_HEADER  0xAA55
#define UART_TO         5000

#define RESP_SUCCESS    0x00
#define RESP_ERROR      0x01
#define RESP_CRC_ERROR  0x02
#define RESP_FLASH_ERR  0x03
#define RESP_BAD_CMD    0x04
#define RESP_BAD_PARAM  0x05

static UART_HandleTypeDef *ph = NULL;
static IAP_Packet_TypeDef rx_pkt;
static IAP_Packet_TypeDef tx_pkt;

static void SendResponse(uint8_t cmd, uint8_t status, uint8_t *data, uint16_t len);

/* 计算数据包 CRC32 — 使用统一的 Bootloader_CalculateCRC32 */
static uint32_t CalculatePacketCRC(IAP_Packet_TypeDef *p)
{
    uint8_t buf[1031];
    uint16_t idx = 0;
    buf[idx++] = p->cmd;
    buf[idx++] = (uint8_t)(p->len & 0xFF);
    buf[idx++] = (uint8_t)((p->len >> 8) & 0xFF);
    buf[idx++] = (uint8_t)(p->addr & 0xFF);
    buf[idx++] = (uint8_t)((p->addr >> 8) & 0xFF);
    buf[idx++] = (uint8_t)((p->addr >> 16) & 0xFF);
    buf[idx++] = (uint8_t)((p->addr >> 24) & 0xFF);
    if (p->len) { memcpy(buf + idx, p->data, p->len); idx += (uint16_t)p->len; }
    return Bootloader_CalculateCRC32(buf, idx);
}

/* 接收数据包 */
static uint8_t ReceivePacket(IAP_Packet_TypeDef *p)
{
    uint16_t hdr;
    HAL_StatusTypeDef st;

    st = HAL_UART_Receive(ph, (uint8_t*)&hdr, 2, HAL_MAX_DELAY);
    if (st != HAL_OK || hdr != PACKET_HEADER) return 1;

    st = HAL_UART_Receive(ph, &p->cmd, 1, UART_TO);
    if (st != HAL_OK) return 1;

    st = HAL_UART_Receive(ph, (uint8_t*)&p->len, 2, UART_TO);
    if (st != HAL_OK) return 1;

    st = HAL_UART_Receive(ph, (uint8_t*)&p->addr, 4, UART_TO);
    if (st != HAL_OK) return 1;

    if (p->len > 1024) return 1;

    if (p->len > 0) {
        st = HAL_UART_Receive(ph, p->data, p->len, UART_TO);
        if (st != HAL_OK) return 1;
    }

    uint32_t rcvd_crc;
    st = HAL_UART_Receive(ph, (uint8_t*)&rcvd_crc, 4, UART_TO);
    if (st != HAL_OK) return 1;

    uint32_t calc_crc = CalculatePacketCRC(p);
    if (rcvd_crc != calc_crc) {
        /* CRC 错误, 发送错误响应 */
        SendResponse(p->cmd, RESP_CRC_ERROR, NULL, 0);
        return 1;
    }
    return 0;
}

/* 发送响应 */
static void SendResponse(uint8_t cmd, uint8_t status, uint8_t *data, uint16_t len)
{
    uint16_t hdr = PACKET_HEADER;
    tx_pkt.cmd = cmd;
    tx_pkt.len = len + 1;
    tx_pkt.addr = 0;
    tx_pkt.data[0] = status;
    if (data && len) memcpy(tx_pkt.data + 1, data, len);

    uint32_t crc = CalculatePacketCRC(&tx_pkt);
    HAL_UART_Transmit(ph, (uint8_t*)&hdr, 2, UART_TO);
    HAL_UART_Transmit(ph, &tx_pkt.cmd, 1, UART_TO);
    HAL_UART_Transmit(ph, (uint8_t*)&tx_pkt.len, 2, UART_TO);
    HAL_UART_Transmit(ph, (uint8_t*)&tx_pkt.addr, 4, UART_TO);
    HAL_UART_Transmit(ph, tx_pkt.data, tx_pkt.len, UART_TO);
    HAL_UART_Transmit(ph, (uint8_t*)&crc, 4, UART_TO);
}

/* ---- 公开接口 ---- */
void Bootloader_UART_Init(UART_HandleTypeDef *huart) { ph = huart; }

void Bootloader_UART_SendString(const char *s)
{
    if (ph) HAL_UART_Transmit(ph, (uint8_t *)s, (uint16_t)strlen(s), UART_TO);
}

void Bootloader_UART_Process(void)
{
    if (!ph || !ph->Instance) return;
    if (ph->gState == HAL_UART_STATE_RESET) return;

    if (ReceivePacket(&rx_pkt) != 0) return;

    IAP_StatusTypeDef st;
    uint8_t resp = RESP_SUCCESS;

    switch (rx_pkt.cmd) {
    case IAP_CMD_GET_INFO: {
        Bootloader_InfoTypeDef info;
        Bootloader_GetInfo(&info);
        SendResponse(IAP_CMD_GET_INFO, RESP_SUCCESS, (uint8_t*)&info, sizeof(info));
        return;
    }
    case IAP_CMD_START_UPDATE:
        st = IAP_StartUpdate();
        resp = (st == IAP_SUCCESS) ? RESP_SUCCESS : RESP_FLASH_ERR;
        break;
    case IAP_CMD_WRITE:
        st = IAP_WriteData(rx_pkt.addr, rx_pkt.data, rx_pkt.len);
        resp = (st == IAP_SUCCESS) ? RESP_SUCCESS :
               (st == IAP_FLASH_ERROR) ? RESP_FLASH_ERR : RESP_ERROR;
        break;
    case IAP_CMD_END_UPDATE:
        st = IAP_EndUpdate();
        resp = (st == IAP_SUCCESS) ? RESP_SUCCESS : RESP_ERROR;
        SendResponse(IAP_CMD_END_UPDATE, resp, NULL, 0);
        if (st == IAP_SUCCESS) { HAL_Delay(200); NVIC_SystemReset(); }
        return;
    case IAP_CMD_JUMP:
        SendResponse(IAP_CMD_JUMP, RESP_SUCCESS, NULL, 0);
        HAL_Delay(200);
        NVIC_SystemReset();
        return;
    case IAP_CMD_ERASE:
        st = Bootloader_Flash_Erase(rx_pkt.addr, rx_pkt.len);
        resp = (st == IAP_SUCCESS) ? RESP_SUCCESS : RESP_FLASH_ERR;
        break;
    default:
        resp = RESP_BAD_CMD;
        break;
    }
    SendResponse(rx_pkt.cmd, resp, NULL, 0);
}
