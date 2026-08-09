/**
 * @file    flash_if.h
 * @brief   Flash 操作接口 — 被 BootLoader 和 APP1 共用
 */

#ifndef __FLASH_IF_H
#define __FLASH_IF_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

/**
 * @brief  擦除指定范围的 Flash 扇区
 * @param  sector_start  起始扇区号 (FLASH_SECTOR_0 ~ FLASH_SECTOR_11)
 * @param  sector_count  扇区数量
 * @retval 0=成功, 非0=失败扇区号
 */
int flash_erase_sectors(uint32_t sector_start, uint32_t sector_count);

/**
 * @brief  按字(32-bit)写入 Flash (必须先 erase 再 write)
 * @param  addr  目标地址 (必须 4 字节对齐)
 * @param  data  32-bit 数据
 * @retval HAL_OK / HAL_ERROR
 */
int flash_write_word(uint32_t addr, uint32_t data);

/**
 * @brief  批量写入 (按字)
 * @param  addr   起始地址 (4 字节对齐)
 * @param  buf    源数据缓冲区
 * @param  words  要写入的 32-bit 字数
 * @retval HAL_OK / HAL_ERROR
 */
int flash_write_buf(uint32_t addr, const uint32_t *buf, uint32_t words);

/**
 * @brief  计算 CRC32 (用于固件校验)
 * @param  buf   数据缓冲区
 * @param  len   字节长度
 * @retval CRC32 值
 */
uint32_t flash_crc32(const uint8_t *buf, uint32_t len);

#endif /* __FLASH_IF_H */
