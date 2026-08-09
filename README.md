# STM32F407 + VS1003B 音频播放器

这是一个基于 **STM32F407VGTx** 和 **VS1003B** 的嵌入式音频播放器项目。播放器从 MicroSD 卡读取 MP3/WAV 文件，通过 VS1003B 完成硬件解码，并使用 1.47 英寸 LCD 显示歌曲信息。

仓库同时包含播放器应用程序和 Bootloader，支持从 SD 卡或串口升级固件。

## 主要功能

- 从 MicroSD 卡根目录自动扫描并循环播放 MP3、WAV 文件
- VS1003B 硬件音频解码，SPI 流式传输音频数据
- FreeRTOS 多任务管理音频、SD 卡、LCD 和串口
- LCD 显示歌曲名称、曲目序号和错误状态
- 短按按键切换下一首，长按 2 秒删除当前歌曲
- 支持 SD 卡、YModem 串口和运行时串口命令升级固件
- Bootloader 检查固件合法性并负责 APP2 到 APP1 的安全复制

> [!WARNING]
> 长按 `BTN_NEXT` 2 秒会直接删除 SD 卡中的当前歌曲，操作不可撤销。

## 硬件与软件

### 主要硬件

| 模块 | 说明 |
|---|---|
| MCU | STM32F407VGTx，1 MB Flash |
| 音频解码 | VS1003B |
| 存储 | MicroSD 卡，SPI 模式，建议 FAT32 |
| 显示 | 1.47 英寸 SPI LCD，320 × 172 |
| 调试与升级 | USART1，115200，8-N-1 |
| 下载器 | ST-Link 或 J-Link |

### 开发环境

- Keil MDK-ARM
- STM32CubeMX（需要修改 `.ioc` 时使用）
- ARM Compiler 5/6，按 Keil 工程当前配置选择
- Python 3 和 `pyserial`（串口传输时使用）

安装 Python 依赖：

```bash
python -m pip install pyserial
```

## 仓库结构

```text
music-player/
├── application/                 播放器主程序
│   ├── app/                     音频、SD、LCD、YModem 等业务代码
│   ├── Core/                    STM32CubeMX 生成的核心代码
│   ├── Drivers/                 STM32 HAL 与 CMSIS
│   ├── FATFS/                   FatFs 适配层
│   ├── Middlewares/             FreeRTOS 与 FatFs
│   ├── lcd/                     LCD 驱动
│   ├── tools/                   串口传输和升级工具
│   └── MDK-ARM/vs1003b.uvprojx  Keil 应用工程
├── bootloader/                  Bootloader 程序
│   ├── bootloader/              Bootloader 核心与串口协议
│   ├── Core/                    启动和外设代码
│   └── MDK-ARM/bootloader.uvprojx
└── README.md
```

## 主要引脚连接

### VS1003B（SPI1）

| VS1003B 信号 | STM32F407 引脚 |
|---|---|
| SCK | PA5 |
| MISO | PA6 |
| MOSI / SDI | PA7 |
| XCS | PA4 |
| XDCS | PA3 |
| DREQ | PB0 |
| XRST | PB1 |

### MicroSD（SPI2）

| MicroSD 信号 | STM32F407 引脚 |
|---|---|
| SCK | PB10 |
| MISO | PB14 |
| MOSI | PC3 |
| CS | PC1 |

### LCD 与其他接口

| 功能 | STM32F407 引脚 |
|---|---|
| LCD SPI3 SCK | PB3 |
| LCD SPI3 MOSI | PB5 |
| LCD DC | PB4 |
| LCD PWR | PD3 |
| LCD RST | PD4 |
| LCD CS | PD7 |
| BTN_NEXT | PC13，低电平有效 |
| USART1 TX / RX | PA9 / PA10 |

接线和 PCB 设计还可参考 [`application/hardware`](application/hardware)。实际制作硬件前，请再次核对所用 VS1003B、LCD 和 MicroSD 模块的供电电压及引脚顺序。

## 编译与首次烧录

### 1. 编译 Bootloader

1. 使用 Keil 打开 `bootloader/MDK-ARM/bootloader.uvprojx`。
2. 编译工程。
3. 使用 ST-Link/J-Link 将 Bootloader 烧录到 `0x08000000`。

Bootloader 的可用空间为 64 KB，对应 STM32F407 的 Sector 0～3。

### 2. 编译播放器应用

1. 使用 Keil 打开 `application/MDK-ARM/vs1003b.uvprojx`。
2. 编译工程。
3. 将应用烧录到 `0x08010000`。

应用工程已经通过链接脚本把起始地址设置为 `0x08010000`，并在启动时设置 `SCB->VTOR = 0x08010000`。

> [!CAUTION]
> 烧录应用时不要执行整片擦除，否则会同时清除位于 `0x08000000` 的 Bootloader。首次使用时建议先烧录 Bootloader，再烧录应用。

### 3. 准备 SD 卡

1. 将 SD 卡格式化为 FAT32。
2. 把 `.mp3` 或 `.wav` 文件放到 SD 卡根目录。
3. 如需显示中文文件名，把 `FONT.DZK` 放到 SD 卡根目录。
4. 插入 SD 卡并重新上电。

固件最多扫描根目录中的 64 首歌曲。当前版本不递归扫描子目录。

## 播放器操作

| 操作 | 功能 |
|---|---|
| 正常上电 | 自动挂载 SD 卡并循环播放 |
| 短按 BTN_NEXT | 切换下一首 |
| 长按 BTN_NEXT 2 秒 | 删除当前歌曲 |
| 串口发送 `#INFO#` | 查看应用信息 |
| 串口发送 `#SEND#` | 通过 YModem 接收文件并写入 SD 卡 |
| 串口发送 `#UPDATE#` | 进入运行时固件升级模式 |

串口参数为 **115200、8 数据位、1 停止位、无校验、无流控**。串口命令末尾需要发送回车或换行。

## 固件升级

### 方式一：使用 SD 卡升级（推荐）

1. 编译应用，得到应用二进制文件 `APP1.bin`。
2. 将其重命名为 `UPDATE.bin`，放到 SD 卡根目录。
3. 把 SD 卡插入正在运行的播放器并重新上电。
4. 应用把固件写入 APP2 区并设置升级标志，然后自动重启。
5. Bootloader 将 APP2 固件复制到 APP1，清除标志并启动新程序。

升级完成后，设备会自动删除 SD 卡中的 `UPDATE.bin`，防止重复升级。

### 方式二：开机按键进入 YModem 升级

1. 上电时按住 `BTN_NEXT` 至少 2 秒。
2. 使用 USB 转串口连接 USART1。
3. 执行：

```bash
python application/tools/ymodem_send.py COM3 path/to/APP1.bin
```

把 `COM3` 替换为实际串口号。传输完成后设备会自动重启并由 Bootloader 完成升级。

### 方式三：运行时串口升级

1. 播放器正常运行时，通过串口发送 `#UPDATE#` 加回车。
2. 等待设备输出 `READY`。
3. 运行 `ymodem_send.py` 发送 `APP1.bin`。
4. 接收完成后设备自动重启。

## Flash 分区

| 区域 | 地址范围 | 大小 | 用途 |
|---|---|---:|---|
| Bootloader | `0x08000000` ～ `0x0800FFFF` | 64 KB | 启动、检查升级、跳转应用 |
| APP1 | `0x08010000` ～ `0x0805FFFF` | 320 KB | 当前运行的播放器固件 |
| APP2 | `0x08060000` ～ `0x080BFFFF` | 384 KB | 新固件暂存区，含 16 字节固件头 |
| 参数区 | `0x080C0000` ～ `0x080FFFFF` | 256 KB | 升级标志与预留参数 |

升级标志位于 `0x080FFFF0`，魔法值为 `0xAAAA5555`。Bootloader 会检查固件头、固件大小、初始栈指针和复位向量后再执行更新。

## PC 端工具

| 工具 | 用途 |
|---|---|
| `application/tools/ymodem_send.py` | 通过 YModem 发送应用固件 |
| `application/tools/send_file.py` | 通过 YModem 向 SD 卡发送音频文件 |
| `application/tools/iap_upload.py` | 使用 Bootloader 二进制串口协议升级 |
| `application/gen_update.py` | 生成升级相关文件 |

示例：

```bash
python application/tools/send_file.py COM3 song.mp3
python application/tools/iap_upload.py -p COM3 -f APP1.bin -b 115200
```

## 常见问题

### 上电后没有歌曲

- 确认 SD 卡已经格式化为 FAT32。
- 确认 MP3/WAV 文件位于 SD 卡根目录。
- 检查 SPI2 和 `SD_CS` 接线。

### VS1003B 无声音

- 检查 DREQ、XCS、XDCS 和 XRST 接线。
- 确认 VS1003B 模块供电和音频输出方式正确。
- 检查 LCD 是否显示 VS1003B 相关错误码。

### 中文文件名显示异常

- 确认 SD 卡根目录存在正确的 `FONT.DZK`。
- 当前 LCD 中文显示使用 GBK 字库，文件名编码需与字库兼容。

### 应用能单独运行但经过 Bootloader 后异常

- 确认应用链接地址为 `0x08010000`。
- 确认应用启动时设置了向量表偏移。
- 确认没有使用整片擦除覆盖 Bootloader。

## 相关文档

- [`application/IAP_BUILD.md`](application/IAP_BUILD.md)：更详细的 IAP 构建与升级说明
- [`application/hardware`](application/hardware)：硬件框图和连接资料

## 第三方组件

本项目包含 STM32 HAL、CMSIS、FreeRTOS 和 FatFs。这些组件分别遵循其目录中声明的许可证。仓库目前未单独声明项目代码许可证。
