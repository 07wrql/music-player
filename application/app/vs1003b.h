/**
 * @file    vs1003b.h
 * @brief   VS1003B MP3/WAV Audio Decoder Driver
 *
 * Pin mapping (STM32F407 → VS1003B):
 *   SPI1_SCK  (PA5) → SCLK
 *   SPI1_MISO (PA6) → MISO
 *   SPI1_MOSI (PA7) → MOSI
 *   PA4            → XCS   (command chip select)
 *   PA3            → XDCS  (data chip select)
 *   PB0            → DREQ  (data request, input)
 *   PB1            → XRESET (hardware reset)
 */

#ifndef __VS1003B_H
#define __VS1003B_H

#include "stm32f4xx_hal.h"

/* ================================================================
   Register Addresses (SCI — Serial Command Interface)
   ================================================================ */
#define VS_WRITE_COMMAND    0x02
#define VS_READ_COMMAND     0x03

#define SPI_MODE            0x00    /* Mode control */
#define SPI_STATUS          0x01    /* Status */
#define SPI_BASS            0x02    /* Bass/treble enhancement */
#define SPI_CLOCKF          0x03    /* Clock frequency */
#define SPI_DECODE_TIME     0x04    /* Decode time (read-only) */
#define SPI_AUDATA          0x05    /* Audio data / misc */
#define SPI_WRAM            0x06    /* Workspace RAM access */
#define SPI_WRAMADDR        0x07    /* Workspace RAM address */
#define SPI_HDAT0           0x08    /* Stream header data 0 */
#define SPI_HDAT1           0x09    /* Stream header data 1 */
#define SPI_AIADDR          0x0A    /* Application start address */
#define SPI_VOL             0x0B    /* Volume control */
#define SPI_AICTRL0         0x0C    /* Application control 0 */
#define SPI_AICTRL1         0x0D    /* Application control 1 */
#define SPI_AICTRL2         0x0E    /* Application control 2 */
#define SPI_AICTRL3         0x0F    /* Application control 3 */

/* ================================================================
   SCI_MODE bit definitions
   ================================================================ */
#define SM_DIFF             0x0001  /* Differential output */
#define SM_JUMP             0x0002  /* Fast forward (jump to next track) */
#define SM_RESET            0x0004  /* Soft reset */
#define SM_OUTOFWAV         0x0008  /* Out of WAV data */
#define SM_PDOWN            0x0010  /* Power down */
#define SM_TESTS            0x0020  /* Enable sine test */
#define SM_STREAM           0x0040  /* Stream mode */
#define SM_PLUSV            0x0080  /* Enable VSD */
#define SM_DACT             0x0100  /* DCLK active edge */
#define SM_SDIORD           0x0200  /* SDI read order */
#define SM_SDISHARE         0x0400  /* Share SDI chip select */
#define SM_SDINEW           0x0800  /* VS1002 native SDI mode */
#define SM_ADPCM            0x1000  /* ADPCM recording active */
#define SM_ADPCM_HP         0x2000  /* ADPCM high-pass filter */

/* ================================================================
   Hardware Pin Macros
   ================================================================ */
#define VS1003_SPI_HANDLE       hspi1

/* XRESET — hardware reset (PB1, active low) */
#define VS1003_XRESET_PORT      GPIOB
#define VS1003_XRESET_PIN       GPIO_PIN_1

/* XCS — command chip select (PA4, active low) */
#define VS1003_XCS_PORT         GPIOA
#define VS1003_XCS_PIN          GPIO_PIN_4

/* XDCS — data chip select (PA3, active low) */
#define VS1003_XDCS_PORT        GPIOA
#define VS1003_XDCS_PIN         GPIO_PIN_3

/* DREQ — data request input (PB0, high = ready) */
#define VS1003_DREQ_PORT        GPIOB
#define VS1003_DREQ_PIN         GPIO_PIN_0

/* ---- Convenience macros ---- */
#define Mp3PutInReset()         HAL_GPIO_WritePin(VS1003_XRESET_PORT, VS1003_XRESET_PIN, GPIO_PIN_RESET)
#define Mp3ReleaseFromReset()   HAL_GPIO_WritePin(VS1003_XRESET_PORT, VS1003_XRESET_PIN, GPIO_PIN_SET)
#define Mp3SelectControl()      HAL_GPIO_WritePin(VS1003_XCS_PORT,  VS1003_XCS_PIN,  GPIO_PIN_RESET)
#define Mp3DeselectControl()    HAL_GPIO_WritePin(VS1003_XCS_PORT,  VS1003_XCS_PIN,  GPIO_PIN_SET)
#define Mp3SelectData()         HAL_GPIO_WritePin(VS1003_XDCS_PORT, VS1003_XDCS_PIN, GPIO_PIN_RESET)
#define Mp3DeselectData()       HAL_GPIO_WritePin(VS1003_XDCS_PORT, VS1003_XDCS_PIN, GPIO_PIN_SET)
#define Mp3GetDREQ()            HAL_GPIO_ReadPin(VS1003_DREQ_PORT,  VS1003_DREQ_PIN)
#define Mp3SetVolume(l, r)      Mp3WriteRegister(SPI_VOL, (l), (r))

/* ================================================================
   Debug / Diagnostics
   ================================================================ */
#define VS1003_ERR_NONE              0
#define VS1003_ERR_DREQ_TIMEOUT      1
#define VS1003_ERR_NO_DREQ_AFTER_RESET 2
#define VS1003_ERR_INIT_FAILED       3

typedef struct {
    uint32_t init_count;            /* VS1003_Init 调用次数 */
    uint32_t hw_reset_count;        /* 硬件复位次数 */
    uint32_t soft_reset_count;      /* 软件复位次数 */
    uint32_t reg_writes;            /* SCI 写寄存器次数 */
    uint32_t reg_reads;             /* SCI 读寄存器次数 */
    uint32_t dreq_write_timeouts;   /* 写寄存器时 DREQ 超时 */
    uint32_t dreq_read_timeouts;    /* 读寄存器时 DREQ 超时 */
    uint32_t dreq_data_timeouts;    /* 发送数据时 DREQ 超时 */
    uint32_t data_chunks_sent;      /* 发送了多少个 32 字节块 */
    uint32_t data_bytes_sent;       /* 累计发送了多少字节 */
    uint32_t last_error;            /* 最后一次错误码 */
    uint8_t  current_volume;        /* 当前音量 */
    uint8_t  stream_mode_enabled;   /* SM_STREAM 是否已设置 */
} VS1003_Debug;

extern volatile VS1003_Debug vs1003_dbg;

/* ================================================================
   Public API
   ================================================================ */
uint8_t  VS1003_Init(void);
uint8_t  VS1003_Reset(void);
uint8_t  VS1003_SoftReset(void);

uint8_t  Mp3WriteRegister(uint8_t addr, uint8_t high, uint8_t low);
uint16_t Mp3ReadRegister(uint8_t addr);

void     VS1003_SendData(uint8_t *buf, uint16_t len);
uint8_t  VS1003_WaitDREQ(uint32_t timeout);

void     VS1003_SetVolume(uint8_t left, uint8_t right);
void     VS1003_SetBass(uint8_t amp, uint8_t freq);

uint8_t  VS1003_SetPlayMode(void);
#endif /* __VS1003B_H */
