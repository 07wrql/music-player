/**
 * @file    iap_config.h
 * @brief   IAP (In-Application Programming) Flash 分区与共享定义
 *
 * STM32F407VGTx Flash 总容量 1MB, 12个扇区:
 *   Sector 0-3:   16KB each  (共64KB)   @ 0x08000000
 *   Sector 4:     64KB                 @ 0x08010000
 *   Sector 5-11:  128KB each (共896KB)
 *
 * 分区方案 (扇区边界完美对齐):
 *   BootLoader  0x08000000 ~ 0x0800FFFF  ( 64KB, S0-S3)
 *   APP1        0x08010000 ~ 0x0805FFFF  (320KB, S4-S6)
 *   APP2        0x08060000 ~ 0x080BFFFF  (384KB, S7-S9)
 *   参数存储区  0x080C0000 ~ 0x080FFFFF  (256KB, S10-S11)
 */

#ifndef __IAP_CONFIG_H
#define __IAP_CONFIG_H

#include <stdint.h>

/* ================================================================
   Flash 基地址
   ================================================================ */
#define FLASH_BASE_ADDR         0x08000000UL
#define FLASH_TOTAL_SIZE        0x00100000UL   /* 1MB */

/* ================================================================
   BootLoader (S0-S3, 4×16KB = 64KB)
   ================================================================ */
#define BOOTLOADER_BASE         0x08000000UL
#define BOOTLOADER_SIZE         0x00010000UL   /* 64KB */
#define BOOTLOADER_SECTOR_START FLASH_SECTOR_0
#define BOOTLOADER_SECTOR_COUNT 4

/* ================================================================
   APP1 — 运行区 (S4-S6, 64KB+128KB+128KB = 320KB)
   ================================================================ */
#define APP1_BASE               0x08010000UL
#define APP1_SIZE               0x00050000UL   /* 320KB */
#define APP1_SECTOR_START       FLASH_SECTOR_4
#define APP1_SECTOR_COUNT       3              /* S4=64KB, S5=128KB, S6=128KB */

/* ================================================================
   APP2 — 升级暂存区 (S7-S9, 128KB×3 = 384KB)
   ================================================================ */
#define APP2_BASE               0x08060000UL
#define APP2_SIZE               0x00060000UL   /* 384KB */
#define APP2_SECTOR_START       FLASH_SECTOR_7
#define APP2_SECTOR_COUNT       3              /* S7=128KB, S8=128KB, S9=128KB */

/* ================================================================
   参数存储区 (S10-S11, 128KB×2 = 256KB)
   用于存储: 升级标志、固件CRC、版本号、播放器配置等
   ================================================================ */
#define PARAM_BASE              0x080C0000UL
#define PARAM_SIZE              0x00040000UL   /* 256KB */
#define PARAM_SECTOR_START      FLASH_SECTOR_10
#define PARAM_SECTOR_COUNT      2              /* S10=128KB, S11=128KB */

/* ================================================================
   更新标志 — 存放在参数区最后一个扇区末尾
   地址: 0x080FFFF0 (S11 最后 16 字节, 不会被任何固件覆盖)
   ================================================================ */
#define UPDATE_FLAG_ADDR        0x080FFFF0UL
#define UPDATE_FLAG_MAGIC       0xAAAA5555UL   /* "有新固件待升级" */

/* ================================================================
   固件镜像头 (放在 APP2 开头, 用于 BootLoader 校验)
   结构体大小: 16 字节
   ================================================================ */
#define FW_HEADER_MAGIC         0x46535550UL   /* "FSUP" = Firmware Setup */

typedef struct
{
    uint32_t magic;         /* FW_HEADER_MAGIC — 固件有效性标识 */
    uint32_t fw_size;       /* 固件二进制大小 (字节), 不含此头部 */
    uint32_t fw_version;    /* 版本号 (如 0x00010001 = v1.0.1) */
    uint32_t fw_crc;        /* 整个固件的 CRC32 校验值 (0=跳过校验) */
} fw_header_t;

#define FW_HEADER_SIZE       sizeof(fw_header_t)  /* 16 bytes */

/* ================================================================
   RAM 范围 (STM32F407VGTx: 128KB SRAM1 + 64KB SRAM2)
   用于 BootLoader 校验固件的栈指针是否合法
   ================================================================ */
#define RAM_BASE             0x20000000UL
#define RAM_END              0x20030000UL   /* 192KB total SRAM */

/* ================================================================
   辅助宏
   ================================================================ */
#define IS_VALID_RAM_ADDR(addr)  (((addr) >= RAM_BASE) && ((addr) < RAM_END))

#define IS_VALID_FLASH_ADDR(addr) \
    (((addr) >= FLASH_BASE_ADDR) && ((addr) < (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE)))

#define IS_VALID_APP1_CODE_ADDR(addr) \
    (((addr) >= APP1_BASE) && ((addr) < (APP1_BASE + APP1_SIZE)))

/* APP 固件最大大小 (不含头部) */
#define APP_FW_MAX_SIZE       (APP1_SIZE)

#endif /* __IAP_CONFIG_H */
