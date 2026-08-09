/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * @file    user_diskio.c
  * @brief   SD Card SPI driver for FatFs (STM32F407 + VS1003B project)
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

#ifdef USE_OBSOLETE_USER_CODE_SECTION_0
/*
 * Warning: the user section 0 is no more in use (starting from CubeMx version 4.16.0)
 * To be suppressed in the future.
 * Kept to ensure backward compatibility with previous CubeMx versions when
 * migrating projects.
 * User code previously added there should be copied in the new user sections before
 * the section contents can be deleted.
 */
/* USER CODE BEGIN 0 */
/* USER CODE END 0 */
#endif

/* USER CODE BEGIN DECL */

/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include "ff_gen_drv.h"
#include "spi.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

/* SD卡片选引脚 PC1 */
#define SD_CS_GPIO_Port  GPIOC
#define SD_CS_Pin        GPIO_PIN_1

/* SD Card SPI commands */
#define CMD0    0x00    /* GO_IDLE_STATE - 复位 */
#define CMD1    0x01    /* SEND_OP_COND - 初始化v1.x卡 */
#define CMD8    0x08    /* SEND_IF_COND - 检测SDHC */
#define CMD9    0x09    /* SEND_CSD */
#define CMD10   0x0A    /* SEND_CID */
#define CMD12   0x0C    /* STOP_TRANSMISSION */
#define CMD16   0x10    /* SET_BLOCKLEN - 设置块大小 */
#define CMD17   0x11    /* READ_SINGLE_BLOCK */
#define CMD18   0x12    /* READ_MULTIPLE_BLOCK */
#define CMD23   0x17    /* SET_BLOCK_COUNT */
#define CMD24   0x18    /* WRITE_BLOCK */
#define CMD25   0x19    /* WRITE_MULTIPLE_BLOCK */
#define CMD55   0x37    /* APP_CMD - 下一条命令为ACMD */
#define CMD58   0x3A    /* READ_OCR */
#define ACMD41  0x29    /* SD_SEND_OP_COND - 初始化v2.x卡 */

/* 片选宏: 低电平有效 */
#define SD_CS_LOW()   HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_RESET)
#define SD_CS_HIGH()  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin, GPIO_PIN_SET)

/* SD卡类型 */
#define SD_TYPE_UNKNOWN  0
#define SD_TYPE_V1       1
#define SD_TYPE_V2       2
#define SD_TYPE_V2HC     3

/* 超时次数 */
#define SD_CMD_TIMEOUT    200
#define SD_INIT_TIMEOUT   1000

/* Private variables ---------------------------------------------------------*/
extern SPI_HandleTypeDef hspi2;

static volatile DSTATUS Stat = STA_NOINIT;
static uint8_t  sd_type = SD_TYPE_UNKNOWN;

/* ---- SD Card SPI helpers (in DECL so CubeMX preserves them) ---- */

static uint8_t spi_txrx(uint8_t tx)
{
    uint8_t rx;
    HAL_SPI_TransmitReceive(&hspi2, &tx, &rx, 1, 100);
    return rx;
}

static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t  buf[6];
    uint8_t  resp;
    uint16_t timeout;

    buf[0] = cmd | 0x40;
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)(arg);
    /* CMD0  CRC=0x95, CMD8  CRC=0x87 (SPI mode requires valid CRC for these),
     * other commands CRC is ignored by the card */
    buf[5] = (cmd == CMD0) ? 0x95 : (cmd == CMD8) ? 0x87 : 0x01;

    SD_CS_HIGH();   /* 每次命令前先拉高CS，确保干净起始 */
    spi_txrx(0xFF);
    SD_CS_LOW();
    HAL_SPI_Transmit(&hspi2, buf, 6, 100);

    timeout = SD_CMD_TIMEOUT;
    do {
        resp = spi_txrx(0xFF);
    } while ((resp & 0x80) && --timeout);

    return resp;
}

static uint8_t sd_send_acmd(uint8_t acmd, uint32_t arg)
{
    uint8_t r1 = sd_send_cmd(CMD55, 0);
    if (r1 > 1) return r1;
    return sd_send_cmd(acmd, arg);
}

/* USER CODE END DECL */

/* Private function prototypes -----------------------------------------------*/
DSTATUS USER_initialize (BYTE pdrv);
DSTATUS USER_status (BYTE pdrv);
DRESULT USER_read (BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
#if _USE_WRITE == 1
  DRESULT USER_write (BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);
#endif /* _USE_WRITE == 1 */
#if _USE_IOCTL == 1
  DRESULT USER_ioctl (BYTE pdrv, BYTE cmd, void *buff);
#endif /* _USE_IOCTL == 1 */

Diskio_drvTypeDef  USER_Driver =
{
  USER_initialize,
  USER_status,
  USER_read,
#if  _USE_WRITE
  USER_write,
#endif  /* _USE_WRITE == 1 */
#if  _USE_IOCTL == 1
  USER_ioctl,
#endif /* _USE_IOCTL == 1 */
};

/* Private functions ---------------------------------------------------------*/

/**
  * @brief  Initializes a Drive
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_initialize (
	BYTE pdrv           /* Physical drive nmuber to identify the drive */
)
{
  /* USER CODE BEGIN INIT */
    uint8_t  r1, dummy;
    uint16_t to;
    uint8_t  is_sdhc = 0;

    (void)pdrv;

    dbg.sd_init_stage = 0;
    dbg.sd_type = SD_TYPE_UNKNOWN;
    dbg.sd_r1_cmd0 = 0;
    dbg.sd_r1_cmd8 = 0;
    dbg.sd_r1_acmd41 = 0;

    /*
     * ---- 第0步: 硬件自检（不阻塞，仅捕获调试信息） ----
     */
    dbg.cs_pin_state = (GPIOC->IDR >> 1) & 0x1;  /* PC1=CS 实际电平 */
    dbg.sd_r1_cmd0   = GPIOB->IDR;               /* GPIOB 全部引脚快照 */
    dbg.pc2_pupdr    = (GPIOB->PUPDR >> 28) & 0x3;
    dbg.miso_idr_cs_hi = (GPIOB->IDR >> 14) & 0x1;
    dbg.miso_idr_cs_lo  = (GPIOB->IDR >> 12) & 0x1; /* PB12 对照 */

    /*
     * ---- 第1步: 80 个连续时钟脉冲 (CS=HIGH) ----
     * 必须连续不间断，将 SD 卡从 SD 模式切换到 SPI 模式
     */
    dbg.sd_init_stage = 1;
    SD_CS_HIGH();
    for (uint8_t i = 0; i < 10; i++) {
        dummy = 0xFF;
        HAL_SPI_TransmitReceive(&hspi2, &dummy, &dummy, 1, 100);
    }

    /* ---- 第2步: CMD0 (复位到 Idle) ---- */
    dbg.sd_init_stage = 2;
    r1 = sd_send_cmd(CMD0, 0);
    dbg.sd_r1_cmd0 = r1;
    if (r1 != 0x01) { dbg.sd_init_stage = 99; sd_type = SD_TYPE_UNKNOWN; Stat = STA_NOINIT; dbg.sd_init_ret = Stat; return Stat; }

    /* ---- 第3步: CMD8 (检测 SDHC) ---- */
    dbg.sd_init_stage = 3;
    r1 = sd_send_cmd(CMD8, 0x1AA);
    dbg.sd_r1_cmd8 = r1;
    if (r1 == 0x01) {
        /* V2 card: read 4-byte R7 response echo */
        sd_type = SD_TYPE_V2; is_sdhc = 1;
        for (int i = 0; i < 4; i++) {
            dummy = 0xFF; HAL_SPI_TransmitReceive(&hspi2, &dummy, &dummy, 1, 100);
        }
        SD_CS_HIGH();
    } else {
        sd_type = SD_TYPE_V1; is_sdhc = 0;
        SD_CS_HIGH();
    }

    /* ---- 第4步: ACMD41 (等待卡退出 Idle) ---- */
    dbg.sd_init_stage = 4;
    to = SD_INIT_TIMEOUT;
    do {
        r1 = sd_send_acmd(ACMD41, is_sdhc ? 0x40000000UL : 0);
        HAL_Delay(1);
    } while (r1 != 0x00 && --to);
    dbg.sd_r1_acmd41 = r1;
    if (r1 != 0x00) { dbg.sd_init_stage = 44; sd_type = SD_TYPE_UNKNOWN; Stat = STA_NOINIT; dbg.sd_init_ret = Stat; return Stat; }

    /* ---- 第5步: CMD58 检测 SDHC ---- */
    dbg.sd_init_stage = 5;
    if (sd_type == SD_TYPE_V2) {
        uint32_t ocr = 0;
        r1 = sd_send_cmd(CMD58, 0);
        if (r1 == 0x00) {
            for (int i = 0; i < 4; i++) {
                uint8_t b;
                dummy = 0xFF; HAL_SPI_TransmitReceive(&hspi2, &dummy, &b, 1, 100);
                ocr = (ocr << 8) | b;
            }
            if (ocr & 0x40000000) sd_type = SD_TYPE_V2HC;
        }
        SD_CS_HIGH();
    }

    /* ---- 第6步: CMD16 (设置块大小 = 512) ---- */
    dbg.sd_init_stage = 6;
    r1 = sd_send_cmd(CMD16, 512);
    if (r1 != 0x00) { dbg.sd_init_stage = 66; Stat = STA_NOINIT; dbg.sd_init_ret = Stat; return Stat; }

    /* ---- 第7步: 切高速 SPI ---- */
    dbg.sd_init_stage = 7;
    SD_SPI_SpeedHigh();

    /* ---- 第8步: 标记初始化完成 ---- */
    dbg.sd_init_stage = 8;
    dbg.sd_type = sd_type;
    Stat &= ~STA_NOINIT;
    dbg.sd_init_ret = Stat;
    return Stat;
  /* USER CODE END INIT */
}

/**
  * @brief  Gets Disk Status
  * @param  pdrv: Physical drive number (0..)
  * @retval DSTATUS: Operation status
  */
DSTATUS USER_status (
	BYTE pdrv       /* Physical drive number to identify the drive */
)
{
  /* USER CODE BEGIN STATUS */
    (void)pdrv;
    return Stat;
  /* USER CODE END STATUS */
}

/**
  * @brief  Reads Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data buffer to store read data
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to read (1..128)
  * @retval DRESULT: Operation result
  */
DRESULT USER_read (
	BYTE pdrv,      /* Physical drive nmuber to identify the drive */
	BYTE *buff,     /* Data buffer to store read data */
	DWORD sector,   /* Sector address in LBA */
	UINT count      /* Number of sectors to read */
)
{
  /* USER CODE BEGIN READ */
    uint8_t  r1;
    uint16_t timeout;
    DWORD    addr;

    (void)pdrv;

    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (count == 0)        return RES_PARERR;

    /* SDSC (v1/v2): 地址=扇区号*512 (字节寻址)
       SDHC (v2HC): 地址=扇区号 (块寻址) */
    addr = (sd_type == SD_TYPE_V2HC) ? sector : (sector * 512);

    for (UINT i = 0; i < count; i++)
    {
        /* 发送读单块命令 CMD17 */
        r1 = sd_send_cmd(CMD17, addr);
        if (r1 != 0x00) {
            SD_CS_HIGH();
            return RES_ERROR;
        }

        /* 等待数据起始令牌 0xFE */
        timeout = 0xFFFF;
        do {
            r1 = spi_txrx(0xFF);
        } while (r1 == 0xFF && --timeout);

        if (r1 != 0xFE) {
            SD_CS_HIGH();
            return RES_ERROR;
        }

        /* 读取512字节扇区数据 */
        for (uint16_t j = 0; j < 512; j++) {
            buff[j] = spi_txrx(0xFF);
        }

        /* 读取并舍弃2字节CRC */
        spi_txrx(0xFF);
        spi_txrx(0xFF);

        SD_CS_HIGH();

        buff += 512;

        /* SDSC: 地址增加512 (字节寻址) */
        if (sd_type != SD_TYPE_V2HC) {
            addr += 512;
        } else {
            addr++;  /* SDHC: 块寻址，地址+1 */
        }
    }

    return RES_OK;
  /* USER CODE END READ */
}

/**
  * @brief  Writes Sector(s)
  * @param  pdrv: Physical drive number (0..)
  * @param  *buff: Data to be written
  * @param  sector: Sector address (LBA)
  * @param  count: Number of sectors to write (1..128)
  * @retval DRESULT: Operation result
  */
#if _USE_WRITE == 1
DRESULT USER_write (
	BYTE pdrv,          /* Physical drive nmuber to identify the drive */
	const BYTE *buff,   /* Data to be written */
	DWORD sector,       /* Sector address in LBA */
	UINT count          /* Number of sectors to write */
)
{
  /* USER CODE BEGIN WRITE */
    uint8_t  r1;
    DWORD    addr;

    (void)pdrv;

    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (count == 0)        return RES_PARERR;

    addr = (sd_type == SD_TYPE_V2HC) ? sector : (sector * 512);

    for (UINT i = 0; i < count; i++)
    {
        /* 发送写单块命令 CMD24 */
        r1 = sd_send_cmd(CMD24, addr);
        if (r1 != 0x00) {
            SD_CS_HIGH();
            return RES_ERROR;
        }

        /* 发送起始令牌 0xFE */
        spi_txrx(0xFF);      /* 额外时钟 */
        SD_CS_LOW();
        spi_txrx(0xFE);      /* 写起始令牌 */
        spi_txrx(0xFF);
        spi_txrx(0xFF);

        /* 写入512字节 */
        HAL_SPI_Transmit(&hspi2, (uint8_t *)buff, 512, 500);
        buff += 512;

        /* 发送2字节伪CRC */
        spi_txrx(0xFF);
        spi_txrx(0xFF);

        /* 等待写入完成: 卡返回非0xFF */
        uint16_t timeout = 0xFFFF;
        do {
            r1 = spi_txrx(0xFF);
        } while (r1 == 0xFF && --timeout);

        SD_CS_HIGH();

        /* 检查写入完成状态: bit0=Busy, bit1=EraseSeq, bit4=CRC error */
        if ((r1 & 0x05) != 0x05) {  /* 数据已接收且无CRC误差 */
            return RES_ERROR;
        }

        /* 等待卡退出Busy状态 */
        timeout = 0xFFFF;
        while (spi_txrx(0xFF) == 0x00 && --timeout);

        if (sd_type != SD_TYPE_V2HC) {
            addr += 512;
        } else {
            addr++;
        }
    }

    return RES_OK;
  /* USER CODE END WRITE */
}
#endif /* _USE_WRITE == 1 */

/**
  * @brief  I/O control operation
  * @param  pdrv: Physical drive number (0..)
  * @param  cmd: Control code
  * @param  *buff: Buffer to send/receive control data
  * @retval DRESULT: Operation result
  */
#if _USE_IOCTL == 1
DRESULT USER_ioctl (
	BYTE pdrv,      /* Physical drive nmuber (0..) */
	BYTE cmd,       /* Control code */
	void *buff      /* Buffer to send/receive control data */
)
{
  /* USER CODE BEGIN IOCTL */
    DRESULT res = RES_OK;
    uint8_t csd[16];
    uint8_t r1;

    (void)pdrv;

    if (Stat & STA_NOINIT) return RES_NOTRDY;

    switch (cmd)
    {
    case CTRL_SYNC:
        /* 刷新缓存 → SD卡SPI模式不需要额外操作 */
        SD_CS_HIGH();
        res = RES_OK;
        break;

    case GET_SECTOR_COUNT:
        /* 读取CSD寄存器获取总扇区数 */
        r1 = sd_send_cmd(CMD9, 0);   /* SEND_CSD */
        if (r1 != 0x00) { res = RES_ERROR; break; }

        /* 等待数据令牌 */
        {
            uint16_t t = 0xFFFF;
            do { r1 = spi_txrx(0xFF); } while (r1 == 0xFF && --t);
        }
        if (r1 != 0xFE) { res = RES_ERROR; SD_CS_HIGH(); break; }

        for (uint8_t i = 0; i < 16; i++) csd[i] = spi_txrx(0xFF);
        /* 舍弃2字节CRC */
        spi_txrx(0xFF); spi_txrx(0xFF);
        SD_CS_HIGH();

        /* 解析CSD获取容量 */
        if ((csd[0] >> 6) == 1) {
            /* CSD v2.0 (SDHC/SDXC) */
            DWORD c_size = ((DWORD)(csd[7] & 0x3F) << 16)
                         | ((DWORD)csd[8] << 8)
                         |  csd[9];
            *(DWORD *)buff = (c_size + 1) * 1024;  /* 扇区数 */
        } else {
            /* CSD v1.0 (SDSC) */
            DWORD c_size  = ((DWORD)(csd[6] & 0x03) << 10)
                          | ((DWORD)csd[7] << 2)
                          | ((DWORD)csd[8] >> 6);
            DWORD c_mult  = (DWORD)(csd[9] & 0x03) << 1
                          | ((DWORD)csd[10] >> 7);
            DWORD blk_len = (DWORD)(csd[5] & 0x0F);
            DWORD blk_n   = (c_size + 1) * (1UL << (c_mult + 2));
            *(DWORD *)buff = blk_n * (1UL << (blk_len - 9));
        }
        res = RES_OK;
        break;

    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        res = RES_OK;
        break;

    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;  /* 擦除块大小未知，返回1 */
        res = RES_OK;
        break;

    default:
        res = RES_PARERR;
        break;
    }

    return res;
  /* USER CODE END IOCTL */
}
#endif /* _USE_IOCTL == 1 */

