# IAP 双工程构建与升级指南

## Flash 分区 (STM32F407VGTx 1MB)

| 区域 | 起始地址 | 大小 | 扇区 | 说明 |
|------|----------|------|------|------|
| BootLoader | `0x08000000` | 64KB | S0-S3 (4×16KB) | 上电先运行, 检查升级→跳转APP1 |
| APP1 | `0x08010000` | 320KB | S4-S6 (64+128+128KB) | 主程序运行区 (播放器+FreeRTOS) |
| APP2 | `0x08060000` | 384KB | S7-S9 (3×128KB) | 新固件暂存区 |
| 参数区 | `0x080C0000` | 256KB | S10-S11 (2×128KB) | 升级标志、配置参数 |

- 更新标志地址: `0x080FFFF0` (S11末尾)
- 更新魔法值: `0xAAAA5555`

---

## 工程结构 (两个独立 Keil 工程)

### 工程 A: BootLoader (`bootloader.uvprojx`)

**CubeMX 配置要点:**
- 仅开启: RCC (HSE), USART1 (调试用)
- 关闭: SPI, I2C, TIM, GPIO 按键, FreeRTOS, FatFs
- 系统时钟: HSE 25MHz → PLL 168MHz

**工程设置:**
- Linker → Scatter File: `bootloader.sct`
- IROM1: `0x08000000` Size `0x10000`

**源文件清单:**
```
bootloader/
  bootloader_vector.s      ← 向量表 (替代 startup_stm32f407xx.s!)
  bootloader_entry.c        ← main() 入口
  bootloader_main.c         ← BootLoader 核心逻辑
  bootloader_main.h

Core/Inc/
  iap_config.h              ← Flash 分区定义 (共享)

app/
  flash_if.c                ← Flash 操作 (共享)
  flash_if.h

Drivers/
  CMSIS/                    ← 系统文件
  STM32F4xx_HAL_Driver/     ← 仅需: hal, hal_cortex, hal_flash, hal_flash_ex,
                               hal_rcc, hal_rcc_ex, hal_pwr, hal_pwr_ex
```

**关键配置:**
- 排除 `startup_stm32f407xx.s` (使用 `bootloader_vector.s` 替代)
- 排除 `Core/Src/main.c` (使用 `bootloader_entry.c` 的 main)
- 排除所有 FreeRTOS/FatFs/LCD/VS1003 文件

---

### 工程 B: APP 主程序 (`vs1003b.uvprojx`)

**CubeMX 配置要点:**
- 全部外设: SPI1(VS1003), SPI2(SD卡), SPI3(LCD), USART1, GPIO, TIM6
- 开启: FreeRTOS (CMSIS_V2), FatFs (SD卡, SPI模式)
- 系统时钟: HSE 25MHz → PLL 168MHz

**工程设置:**
- Linker → Scatter File: `app_direct.sct`
- IROM1: `0x08010000` Size `0x50000`

**源文件清单:**
```
Core/Src/
  main.c                    ← APP 入口 (含 VTOR 偏移)
  freertos.c                ← FreeRTOS 任务创建
  gpio.c, spi.c, usart.c
  stm32f4xx_it.c
  stm32f4xx_hal_msp.c
  stm32f4xx_hal_timebase_tim.c
  system_stm32f4xx.c

Core/Inc/
  iap_config.h              ← Flash 分区定义 (共享)

app/
  flash_if.c/h              ← Flash 操作 (共享)
  vs1003b.c/h               ← VS1003B 驱动
  wav_player.c/h            ← WAV 播放逻辑
  audio_task.c/h            ← 音频后台任务
  sd_task.c/h               ← SD 卡读写任务
  lcd_task.c/h              ← OLED 显示任务
  ymodem.c/h                ← YModem 协议接收
  uart_recv.c/h             ← UART 命令处理

FATFS/                      ← FatFs 文件系统
lcd/                        ← OLED 驱动
Middlewares/                 ← FreeRTOS + FatFs 源码
Drivers/                     ← HAL 库

MDK-ARM/
  startup_stm32f407xx.s     ← 标准向量表
```

**关键配置:**
- main.c 开头: `SCB->VTOR = APP1_BASE;` (0x08010000)
- 排除整个 `bootloader/` 文件夹
- After Build: `fromelf --bin !L@L#.axf -o ./Objects/APP1.bin`

---

## 首次烧录步骤

1. **烧录 BootLoader:**
   - 打开 BootLoader 工程, 编译 → J-Link/ST-Link 烧录
   - BootLoader 自动烧录到 `0x08000000`

2. **烧录 APP1 (V1.0):**
   - 打开 APP 工程, 编译 → 烧录
   - 确保烧录地址为 `0x08010000` (Keil 根据 scatter file 自动设置)
   - **不要擦除 BootLoader 扇区!**

3. **上电验证:**
   - BootLoader 启动 → 检查无升级标志 → 直接跳转 APP1
   - OLED 显示 "VS1003B Player" → 进入播放器界面

---

## 版本升级流程 (V1.0 → V1.1)

### 方式一: 开机按键进入 IAP 模式
1. 上电时按住 `BTN_NEXT` 键 2 秒
2. OLED 显示 "IAP Update — Waiting PC..."
3. PC 端执行: `python tools/ymodem_send.py COM3 MDK-ARM/vs1003b/APP1.bin`
4. 接收完成 → OLED 显示 "Update OK! Rebooting..."
5. 自动重启 → BootLoader 检测 APP2 固件 → 复制到 APP1 → 跳转新版本

### 方式二: FreeRTOS 运行时串口命令
1. 播放器正常运行中
2. 串口助手发送: `#UPDATE#`
3. 设备回复 `READY` → 用 YModem 发送 `APP1.bin`
4. 接收完成 → 自动复位升级

---

## 容错机制

1. **固件合法性校验:** BootLoader 检查固件头 magic + 栈指针 + PC 地址
2. **断电保护:** 先拷贝完成, 最后才清除升级标志。中途断电重试
3. **双备份:** APP1 始终保留上一版本, 只有拷贝成功才覆盖
4. **防变砖:** BootLoader 独立于 APP, 即使 APP1 损坏也能通过按键触发升级

---

## 扩展: 增加 CRC 校验

在 `bootloader_copy_app2_to_app1()` 中, 拷贝完成后计算 APP1 的 CRC32 与固件头中的 `fw_crc` 比对。
APP 端接收时在 `recv_firmware_to_flash()` 中计算并填入 `hdr.fw_crc`。

---

## 文件清单总览

```
项目根目录/
├── bootloader/              ← BootLoader 专属 (APP工程排除此文件夹)
│   ├── bootloader_vector.s  ← 最小向量表
│   ├── bootloader_entry.c   ← main() 入口
│   ├── bootloader_main.c    ← 核心逻辑
│   └── bootloader_main.h
├── app/                     ← APP 专属 + 共享(flash_if)
│   ├── flash_if.c/h         ← [共享] Flash操作
│   ├── ymodem.c/h           ← YModem协议
│   ├── uart_recv.c/h        ← UART命令
│   ├── vs1003b.c/h          ← VS1003B驱动
│   ├── wav_player.c/h       ← WAV播放
│   ├── audio_task.c/h       ← 音频任务
│   ├── sd_task.c/h          ← SD卡任务
│   └── lcd_task.c/h         ← 显示任务
├── Core/
│   └── Inc/
│       └── iap_config.h     ← [共享] Flash分区定义
├── MDK-ARM/
│   ├── vs1003b.uvprojx      ← APP工程
│   ├── bootloader.uvprojx   ← BootLoader工程 (需新建)
│   ├── startup_stm32f407xx.s
│   └── vs1003b/
│       ├── bootloader.sct
│       ├── app_direct.sct
│       └── vs1003b.sct
└── tools/
    └── ymodem_send.py       ← PC端固件发送工具
```
