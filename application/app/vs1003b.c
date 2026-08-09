/**
 * @file    vs1003b.c
 * @brief   VS1003B Driver Implementation
 *
 * VS1003B uses two SPI interfaces (shared on SPI1):
 *   - SCI (Serial Command Interface):  register read/write via XCS
 *   - SDI (Serial Data Interface):     audio data stream via XDCS
 *
 * DREQ pin signals when VS1003B's internal buffer has ≥32 bytes free.
 * After DREQ goes high, can send up to 32 bytes before checking again.
 */

#include "vs1003b.h"

extern SPI_HandleTypeDef hspi1;

/* 调试计数器 — volatile 防止编译器优化掉 */
volatile VS1003_Debug vs1003_dbg = {0};


/* ================================================================
   Low-Level SPI Helpers
   ================================================================ */

/**
 * @brief  SPI full-duplex byte exchange
 */
static uint8_t VS1003_SPI_ReadWrite(uint8_t data)
{
    uint8_t rx;
    HAL_SPI_TransmitReceive(&VS1003_SPI_HANDLE, &data, &rx, 1, 100);
    return rx;
}

/* ================================================================
   SCI — Register Access
   ================================================================ */

/**
 * @brief  Write to a VS1003B register (SCI)
 * @param  addr  Register address
 * @param  high  High byte of value
 * @param  low   Low byte of value
 * @retval 1 = success, 0 = DREQ timeout (chip not responding)
 */
uint8_t Mp3WriteRegister(uint8_t addr, uint8_t high, uint8_t low)
{
    if (!VS1003_WaitDREQ(2000))
    {
        vs1003_dbg.dreq_write_timeouts++;
        vs1003_dbg.last_error = VS1003_ERR_DREQ_TIMEOUT;
        return 0;
    }
    Mp3DeselectData();
    Mp3SelectControl();
    VS1003_SPI_ReadWrite(VS_WRITE_COMMAND);
    VS1003_SPI_ReadWrite(addr);
    VS1003_SPI_ReadWrite(high);
    VS1003_SPI_ReadWrite(low);
    Mp3DeselectControl();
    vs1003_dbg.reg_writes++;
    return 1;
}

/**
 * @brief  Read from a VS1003B register (SCI)
 * @param  addr  Register address
 * @return 16-bit register value (0xFFFF on DREQ timeout)
 */
uint16_t Mp3ReadRegister(uint8_t addr)
{
    uint8_t h, l;
    if (!VS1003_WaitDREQ(2000))
    {
        vs1003_dbg.dreq_read_timeouts++;
        vs1003_dbg.last_error = VS1003_ERR_DREQ_TIMEOUT;
        return 0xFFFF;
    }
    Mp3DeselectData();
    Mp3SelectControl();
    VS1003_SPI_ReadWrite(VS_READ_COMMAND);
    VS1003_SPI_ReadWrite(addr);
    h = VS1003_SPI_ReadWrite(0xFF);
    l = VS1003_SPI_ReadWrite(0xFF);
    Mp3DeselectControl();
    vs1003_dbg.reg_reads++;
    return ((uint16_t)h << 8) | l;
}

/* ================================================================
   SDI — Audio Data Transfer
   ================================================================ */

/**
 * @brief  Send a buffer of bytes to the data interface (SDI)
 * @param  buf  Data buffer
 * @param  len  Number of bytes to send
 */
void VS1003_SendData(uint8_t *buf, uint16_t len)
{
    uint16_t offset = 0;

    while (offset < len)
    {
        if (VS1003_WaitDREQ(100) == 0)
        {
            vs1003_dbg.dreq_data_timeouts++;
            break;
        }
        uint16_t chunk = (len - offset) >= 32 ? 32 : (len - offset);
        Mp3SelectData();
        HAL_SPI_Transmit(&VS1003_SPI_HANDLE, buf + offset, chunk, 1000);

        /*
         * CRITICAL: wait for SPI shift register to drain before toggling XDCS.
         * HAL_SPI_Transmit only waits for TXE (data->shift reg), not BSY
         * (shift reg->pin).  If we raise XDCS while the last byte is still
         * shifting out, VS1003B sees a partial byte -> permanent misalignment.
         */
        while (__HAL_SPI_GET_FLAG(&VS1003_SPI_HANDLE, SPI_FLAG_BSY));

        Mp3DeselectData();
        offset += chunk;
        vs1003_dbg.data_chunks_sent++;
        vs1003_dbg.data_bytes_sent += chunk;
    }
}

/**
 * @brief  Wait for DREQ pin to go high
 * @param  timeout  Timeout in milliseconds
 * @retval 1 = DREQ went high, 0 = timeout
 */
uint8_t VS1003_WaitDREQ(uint32_t timeout)
{
    uint32_t t = HAL_GetTick();
    while (Mp3GetDREQ() == GPIO_PIN_RESET) {
        if ((HAL_GetTick() - t) > timeout) return 0;
    }
    return 1;
}

/* ================================================================
   Initialization & Reset
   ================================================================ */

/**
 * @brief  Soft reset + configure clock, audio format, bass, volume
 * @retval 1 = success, 0 = DREQ timeout (chip not responding)
 */
uint8_t VS1003_SoftReset(void)
{
    vs1003_dbg.soft_reset_count++;

    /* Set SM_SDINEW | SM_RESET */
    if (!Mp3WriteRegister(SPI_MODE, 0x08, 0x04))
    {
        vs1003_dbg.last_error = VS1003_ERR_INIT_FAILED;
        return 0;
    }
    HAL_Delay(2);

    if (!VS1003_WaitDREQ(200))
    {
        vs1003_dbg.last_error = VS1003_ERR_DREQ_TIMEOUT;
        return 0;
    }

    Mp3WriteRegister(SPI_CLOCKF, 0x60, 0x00);    /* 3.0x multiplier -> 36.864MHz CLKI */
    Mp3WriteRegister(SPI_AUDATA, 0xBB, 0x81);    /* 48kHz stereo */
    Mp3WriteRegister(SPI_BASS, 0x00, 0x55);      /* Bass enhancement default */
    Mp3SetVolume(10, 10);
    HAL_Delay(2);

    /* Send 4 zero bytes to re-sync SPI data interface */
    Mp3SelectData();
    VS1003_SPI_ReadWrite(0x00);
    VS1003_SPI_ReadWrite(0x00);
    VS1003_SPI_ReadWrite(0x00);
    VS1003_SPI_ReadWrite(0x00);
    Mp3DeselectData();

    vs1003_dbg.last_error = VS1003_ERR_NONE;
    return 1;
}

/**
 * @brief  Hardware reset (toggle XRESET)
 * @retval 1 = success, 0 = DREQ timeout (chip not responding)
 */
uint8_t VS1003_Reset(void)
{
    vs1003_dbg.hw_reset_count++;

    Mp3PutInReset();
    HAL_Delay(20);
    Mp3DeselectControl();
    Mp3DeselectData();
    Mp3ReleaseFromReset();
    HAL_Delay(200);

    /* Wait for VS1003B to boot and assert DREQ */
    if (!VS1003_WaitDREQ(2000))
    {
        vs1003_dbg.last_error = VS1003_ERR_NO_DREQ_AFTER_RESET;
        return 0;
    }

    Mp3SetVolume(50, 50);
    return VS1003_SoftReset();
}

/**
 * @brief  Full initialization: release reset, then hardware reset
 * @retval 1 = success, 0 = failed
 */
uint8_t VS1003_Init(void)
{
    vs1003_dbg.init_count++;

    Mp3ReleaseFromReset();
    Mp3DeselectControl();
    Mp3DeselectData();
    HAL_Delay(50);
    return VS1003_Reset();
}

/* ================================================================
   Audio Controls
   ================================================================ */

/**
 * @brief  Set volume
 * @param  left   Left channel  (0 = max, 254 = mute)
 * @param  right  Right channel (0 = max, 254 = mute)
 */
void VS1003_SetVolume(uint8_t left, uint8_t right)
{
    Mp3WriteRegister(SPI_VOL, left, right);
    vs1003_dbg.current_volume = left;
}

/**
 * @brief  Set bass/treble enhancement
 * @param  amp   Amplitude (0-15 dB)
 * @param  freq  Frequency limit (0-15, x10 Hz)
 */
void VS1003_SetBass(uint8_t amp, uint8_t freq)
{
    uint16_t v = ((uint16_t)(amp & 0x0F) << 4) | (freq & 0x0F);
    Mp3WriteRegister(SPI_BASS, 0x00, (uint8_t)v);
}

/* ================================================================
   Streaming Playback Mode
   ================================================================ */

/**
 * @brief  配置SD卡流媒体持续播放模式，开启流解码+默认音量
 * @retval 1 = success, 0 = failed
 */
uint8_t VS1003_SetPlayMode(void)
{
    /*
     * NOTE: caller (StartAudioTask in audio_task.c) calls VS1003_Init() first.
     * We do NOT reset here — just set volume and enable SM_STREAM.
     * SM_SDINEW is already set by VS1003_SoftReset().
     */
    VS1003_SetVolume(0xCC, 0xCC);   /* 20% 音量 (0=最大, 254=静音) */
    uint16_t mode_val = Mp3ReadRegister(SPI_MODE);
    if (mode_val == 0xFFFF)
    {
        vs1003_dbg.last_error = VS1003_ERR_DREQ_TIMEOUT;
        return 0;
    }
    mode_val |= SM_STREAM;
    Mp3WriteRegister(SPI_MODE, (mode_val >> 8) & 0xFF, mode_val & 0xFF);
    vs1003_dbg.stream_mode_enabled = 1;
    return 1;
}
