/**
 * @file    bootloader.h
 * @brief   BootLoader API — 参照参考项目, 适配 STM32F407
 */

#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include "stm32f4xx_hal.h"
#include "iap_config.h"

/* ---- IAP 状态 ---- */
typedef enum {
    IAP_SUCCESS = 0,
    IAP_ERROR,
    IAP_FLASH_ERROR,
    IAP_VERIFY_ERROR,
    IAP_TIMEOUT,
    IAP_INVALID_PARAM,
    IAP_APP_INVALID,
} IAP_StatusTypeDef;

/* ---- BootLoader 信息 ---- */
typedef __packed struct {
    uint8_t  version[3];
    uint32_t bootloader_size;
    uint32_t app_start_addr;
    uint32_t app_max_size;
    uint8_t  mcu_type[16];
} Bootloader_InfoTypeDef;

/* ---- API ---- */
void Bootloader_Init(void);
void Bootloader_GetInfo(Bootloader_InfoTypeDef *info);

uint8_t Bootloader_CheckAppValid(void);
void    Bootloader_JumpToApp(void);

/* APP2 固件检查 / 复制到 APP1（返回 0=成功, 负值=失败原因） */
int bootloader_check_app2(void);
int bootloader_copy_app2_to_app1(void);

IAP_StatusTypeDef Bootloader_Flash_Erase(uint32_t addr, uint32_t size);
IAP_StatusTypeDef Bootloader_Flash_Write(uint32_t addr, uint8_t *data, uint32_t len);

IAP_StatusTypeDef IAP_StartUpdate(void);
IAP_StatusTypeDef IAP_WriteData(uint32_t addr, uint8_t *data, uint32_t len);
IAP_StatusTypeDef IAP_EndUpdate(void);

void    Bootloader_SetUpdateFlag(void);
void    Bootloader_ClearUpdateFlag(void);
uint8_t Bootloader_CheckUpdateFlag(void);

const char* Bootloader_GetStatusString(IAP_StatusTypeDef status);
void Bootloader_ResetDevice(void);
uint32_t Bootloader_CalculateCRC32(uint8_t *data, uint32_t len);

#endif /* __BOOTLOADER_H */
