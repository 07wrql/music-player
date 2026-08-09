/**
 * @file    bootloader.c
 * @brief   BootLoader 实现 — 参照参考项目, 适配 F407
 */

#include "bootloader.h"
#include "flash_if.h"
#include <string.h>

#define COPY_CHUNK_WORDS 1024   /* APP2->APP1 复制块大小 (4KB) */

static uint8_t update_in_progress = 0;
extern UART_HandleTypeDef huart1;

void Bootloader_Init(void)
{
    update_in_progress = 0;
}

void Bootloader_GetInfo(Bootloader_InfoTypeDef *info)
{
    if (!info) return;
    info->version[0] = 1; info->version[1] = 0; info->version[2] = 0;
    info->bootloader_size = BOOTLOADER_SIZE;
    info->app_start_addr  = APP1_BASE;
    info->app_max_size    = APP1_SIZE;
    strcpy((char*)info->mcu_type, "STM32F407VG");
}

/* ================================================================
   APP2 固件检查 / 复制到 APP1（A/B 双区升级核心流程）
   ================================================================ */
int bootloader_check_app2(void)
{
    fw_header_t *hdr = (fw_header_t *)(APP2_BASE);
    if (hdr->magic != FW_HEADER_MAGIC) return 0;
    if (hdr->fw_size == 0 || hdr->fw_size > APP_FW_MAX_SIZE) return 0;

    uint32_t base = APP2_BASE + FW_HEADER_SIZE;
    uint32_t sp = *(volatile uint32_t *)base;
    uint32_t pc = *(volatile uint32_t *)(base + 4);

    if (!IS_VALID_RAM_ADDR(sp)) return 0;
    if (!IS_VALID_APP1_CODE_ADDR(pc & ~1UL)) return 0;
    if ((pc & 1) == 0) return 0;

    return 1;
}

int bootloader_copy_app2_to_app1(void)
{
    fw_header_t *hdr = (fw_header_t *)(APP2_BASE);
    if (!bootloader_check_app2()) return -1;

    uint32_t fw_size = hdr->fw_size;
    uint32_t src_addr = APP2_BASE + FW_HEADER_SIZE;
    uint32_t dst_addr = APP1_BASE;
    uint32_t words    = (fw_size + 3) / 4;

    int r = flash_erase_sectors(APP1_SECTOR_START, APP1_SECTOR_COUNT);
    if (r != 0) return -2;

    /* 按块复制, 减少 HAL_FLASH_Program 调用开销 */
    for (uint32_t i = 0; i < words; i += COPY_CHUNK_WORDS)
    {
        uint32_t n = (words - i < COPY_CHUNK_WORDS) ? (words - i) : COPY_CHUNK_WORDS;
        r = flash_write_buf(dst_addr + i * 4, (const uint32_t *)(src_addr + i * 4), n);
        if (r != 0) return -3;
    }
    return 0;
}

uint8_t Bootloader_CheckAppValid(void)
{
    uint32_t sp = *(volatile uint32_t *)APP1_BASE;
    uint32_t pc = *(volatile uint32_t *)(APP1_BASE + 4);
    if (!IS_VALID_RAM_ADDR(sp)) return 0;
    if (!IS_VALID_APP1_CODE_ADDR(pc & ~1UL)) return 0;
    if ((pc & 1) == 0) return 0;
    return 1;
}

void Bootloader_JumpToApp(void)
{
    if (!Bootloader_CheckAppValid()) return;

    __disable_irq();
    SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;

    for (uint8_t i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    HAL_DeInit();

    uint32_t sp = *(volatile uint32_t *)APP1_BASE;
    uint32_t pc = *(volatile uint32_t *)(APP1_BASE + 4);
    typedef void (*app_t)(void);
    app_t app = (app_t)pc;

    __set_MSP(sp);
    SCB->VTOR = APP1_BASE;
    __DSB(); __ISB();

    app();
}

/* ---- Flash 操作 (F407 用扇区) ---- */
IAP_StatusTypeDef Bootloader_Flash_Erase(uint32_t addr, uint32_t size)
{
    if (addr < APP1_BASE || size == 0) return IAP_INVALID_PARAM;

    /* 计算需要擦除的扇区范围 */
    uint32_t end = addr + size - 1;
    uint32_t s_start, s_end;
    if (addr < 0x08010000) s_start = FLASH_SECTOR_4;
    else if (addr < 0x08020000) s_start = FLASH_SECTOR_4;
    else if (addr < 0x08040000) s_start = FLASH_SECTOR_5;
    else if (addr < 0x08060000) s_start = FLASH_SECTOR_6;
    else if (addr < 0x08080000) s_start = FLASH_SECTOR_7;
    else if (addr < 0x080A0000) s_start = FLASH_SECTOR_8;
    else if (addr < 0x080C0000) s_start = FLASH_SECTOR_9;
    else if (addr < 0x080E0000) s_start = FLASH_SECTOR_10;
    else s_start = FLASH_SECTOR_11;

    if (end < 0x08020000) s_end = FLASH_SECTOR_4;
    else if (end < 0x08040000) s_end = FLASH_SECTOR_5;
    else if (end < 0x08060000) s_end = FLASH_SECTOR_6;
    else if (end < 0x08080000) s_end = FLASH_SECTOR_7;
    else if (end < 0x080A0000) s_end = FLASH_SECTOR_8;
    else if (end < 0x080C0000) s_end = FLASH_SECTOR_9;
    else if (end < 0x080E0000) s_end = FLASH_SECTOR_10;
    else s_end = FLASH_SECTOR_11;

    int r = flash_erase_sectors(s_start, s_end - s_start + 1);
    return (r == 0) ? IAP_SUCCESS : IAP_FLASH_ERROR;
}

IAP_StatusTypeDef Bootloader_Flash_Write(uint32_t addr, uint8_t *data, uint32_t len)
{
    if (addr < APP1_BASE || !data || !len) return IAP_INVALID_PARAM;
    if (addr + len > APP1_BASE + APP1_SIZE) return IAP_INVALID_PARAM;

    /* 按字写入, 末尾不足 4 字节补 0xFF */
    uint32_t words = (len + 3) / 4;
    int r = flash_write_buf(addr, (const uint32_t *)data, words);
    return (r == 0) ? IAP_SUCCESS : IAP_FLASH_ERROR;
}

/* ---- IAP 升级流程 ---- */
IAP_StatusTypeDef IAP_StartUpdate(void)
{
    IAP_StatusTypeDef s = Bootloader_Flash_Erase(APP1_BASE, APP1_SIZE);
    if (s != IAP_SUCCESS) return s;
    update_in_progress = 1;
    Bootloader_SetUpdateFlag();
    return IAP_SUCCESS;
}

IAP_StatusTypeDef IAP_WriteData(uint32_t addr, uint8_t *data, uint32_t len)
{
    if (!update_in_progress) return IAP_ERROR;
    return Bootloader_Flash_Write(addr, data, len);
}

IAP_StatusTypeDef IAP_EndUpdate(void)
{
    update_in_progress = 0;
    if (!Bootloader_CheckAppValid()) return IAP_APP_INVALID;
    Bootloader_ClearUpdateFlag();
    return IAP_SUCCESS;
}

/* ---- 升级标志 ---- */
void Bootloader_SetUpdateFlag(void)
{
    HAL_FLASH_Unlock();
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, UPDATE_FLAG_ADDR, UPDATE_FLAG_MAGIC);
    HAL_FLASH_Lock();
}

void Bootloader_ClearUpdateFlag(void)
{
    flash_erase_sectors(PARAM_SECTOR_START + PARAM_SECTOR_COUNT - 1, 1);
}

uint8_t Bootloader_CheckUpdateFlag(void)
{
    return (*(volatile uint32_t *)UPDATE_FLAG_ADDR == UPDATE_FLAG_MAGIC) ? 1 : 0;
}

const char* Bootloader_GetStatusString(IAP_StatusTypeDef s)
{
    switch (s) {
    case IAP_SUCCESS:       return "Success";
    case IAP_ERROR:         return "Error";
    case IAP_FLASH_ERROR:   return "Flash Error";
    case IAP_VERIFY_ERROR:  return "Verify Error";
    case IAP_TIMEOUT:       return "Timeout";
    case IAP_INVALID_PARAM: return "Invalid Param";
    case IAP_APP_INVALID:   return "APP Invalid";
    default:                return "Unknown";
    }
}

void Bootloader_ResetDevice(void)
{
    NVIC_SystemReset();
}

/* CRC32 — 与 Python iap_upload.py 使用相同算法 */
static const uint32_t crc32_tab[256] = {
    0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,
    0xE963A535,0x9E6495A3,0x0EDB8832,0x79DCB8A4,0xE0D5E91E,0x97D2D988,
    0x09B64C2B,0x7EB17CBD,0xE7B82D07,0x90BF1D91,0x1DB71064,0x6AB020F2,
    0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
    0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,
    0xFA0F3D63,0x8D080DF5,0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,
    0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,0x35B5A8FA,0x42B2986C,
    0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
    0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,
    0xCFBA9599,0xB8BDA50F,0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,
    0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,0x76DC4190,0x01DB7106,
    0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
    0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,
    0x91646C97,0xE6635C01,0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,
    0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,0x65B0D9C6,0x12B7E950,
    0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
    0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,
    0xA4D1C46D,0xD3D6F4FB,0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,
    0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,0x5005713C,0x270241AA,
    0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
    0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,
    0xB7BD5C3B,0xC0BA6CAD,0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,
    0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,0xE3630B12,0x94643B84,
    0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
    0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,
    0x196C3671,0x6E6B06E7,0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,
    0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,0xD6D6A3E8,0xA1D1937E,
    0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
    0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA867DF55,
    0x316E8EEF,0x4669BE79,0xCB61B38C,0xBC66831A,0x256FD2A0,0x5268E236,
    0xCC0C7795,0xBB0B4703,0x220216B9,0x5505262F,0xC5BA3BBE,0xB2BD0B28,
    0x2BB45A92,0x5CB36A04,0xC2D7FFA7,0xB5D0CF31,0x2CD99E8B,0x5BDEAE1D,
    0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,
    0x72076785,0x05005713,0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,
    0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,0x86D3D2D4,0xF1D4E242,
    0x68DDB3F8,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
    0x88085AE6,0xFF0F6A70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,
    0x616BFFD3,0x166CCF45,0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,
    0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,0xAED16A4A,0xD9D65ADC,
    0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
    0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD70693,
    0x54DE5729,0x23D967BF,0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,
    0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D
};

uint32_t Bootloader_CalculateCRC32(uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++)
        crc = crc32_tab[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
