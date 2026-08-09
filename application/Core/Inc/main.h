/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define SD_CS_Pin GPIO_PIN_1
#define SD_CS_GPIO_Port GPIOC
#define VS_XDCS_Pin GPIO_PIN_3
#define VS_XDCS_GPIO_Port GPIOA
#define VS_XCS_Pin GPIO_PIN_4
#define VS_XCS_GPIO_Port GPIOA
#define VS_DREQ_Pin GPIO_PIN_0
#define VS_DREQ_GPIO_Port GPIOB
#define VS_XRST_Pin GPIO_PIN_1
#define VS_XRST_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
typedef uint32_t  u32;
typedef uint16_t u16;
typedef uint8_t  u8;

/* ---- LCD 控制引脚 ---- */
#define LCD_PWR_Pin       GPIO_PIN_3
#define LCD_PWR_GPIO_Port GPIOD
#define LCD_RST_Pin       GPIO_PIN_4
#define LCD_RST_GPIO_Port GPIOD
#define LCD_CS_Pin        GPIO_PIN_7
#define LCD_CS_GPIO_Port  GPIOD
#define LCD_DC_Pin        GPIO_PIN_4
#define LCD_DC_GPIO_Port  GPIOB

/* ---- 按键 ---- */
#define BTN_NEXT_Pin       GPIO_PIN_13
#define BTN_NEXT_GPIO_Port GPIOC

/* ---- SPI3 句柄 (LCD) ---- */
extern SPI_HandleTypeDef hspi3;

/* Keil Watch 窗口添加 "dbg" 即可观察全部状态 */
typedef struct {    uint32_t mount_res;      /* f_mount 返回值 (FRESULT: 0=OK) */
    uint32_t open_res;       /* f_open  返回值 (FRESULT: 0=OK) */
    int32_t  wav_status;     /* 最终 WAV_Status (0=OK, 1=MOUNT, 2=OPEN, 3=READ, 4=DREQ) */
    uint32_t total_read;     /* 累计从 SD 读取的字节数 */
    uint32_t read_loops;     /* f_read 循环次数 */
    uint32_t dreq_timeouts;  /* DREQ 超时次数 */
    uint32_t sd_init_stage;  /* SD初始化到第几步 */
    uint32_t sd_type;        /* SD卡类型: 0=未知 1=V1 2=V2 3=V2HC */
    uint32_t sd_init_ret;    /* USER_initialize 返回值 (0=OK) */
    uint32_t sd_r1_cmd0;     /* CMD0 的 R1 响应 (应为 0x01) */
    uint32_t sd_r1_cmd8;     /* CMD8 的 R1 响应 */
    uint32_t sd_r1_acmd41;   /* ACMD41 的 R1 响应 (应为 0x00) */
    uint32_t miso_idr_cs_hi; /* PB14 IDR: CS=HIGH */
    uint32_t miso_idr_cs_lo; /* PB12 IDR: 对照引脚 */
    uint32_t pc2_pupdr;      /* PUPDR 上拉配置 */
    uint32_t cs_pin_state;   /* PC1 (CS) 电平: 应为 1 */
    uint32_t dir_entries;    /* 根目录找到的文件数 */
    char     dir_name[4][13]; /* 前4个文件名 (8.3格式) */
} WAV_Debug;

extern WAV_Debug dbg;

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
