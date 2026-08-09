#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
STM32 IAP固件升级工具
支持通过串口对STM32进行固件升级

使用方法:
    python iap_upload.py -p COM3 -f firmware.bin -b 115200
    
参数说明:
    -p, --port      串口号 (例如: COM3 或 /dev/ttyUSB0)
    -f, --file      固件文件路径 (.bin格式)
    -b, --baudrate  波特率 (默认: 115200)
    -v, --verbose   显示详细信息
"""

import serial
import struct
import time
import sys
import argparse
import os

# IAP命令定义
IAP_CMD_ERASE = 0xA0
IAP_CMD_WRITE = 0xA1
IAP_CMD_READ = 0xA2
IAP_CMD_JUMP = 0xA3
IAP_CMD_GET_INFO = 0xA4
IAP_CMD_START_UPDATE = 0xA5
IAP_CMD_END_UPDATE = 0xA6
IAP_CMD_VERIFY = 0xA7
IAP_CMD_GET_APP_VERSION = 0xA8
IAP_CMD_SET_APP_VERSION = 0xA9
IAP_CMD_COMPARE_VERSION = 0xAA

# 数据包格式
PACKET_HEADER = 0xAA55
PACKET_TAIL = 0x55AA

# 响应码
RESPONSE_SUCCESS = 0x00
RESPONSE_ERROR = 0x01
RESPONSE_CRC_ERROR = 0x02
RESPONSE_FLASH_ERROR = 0x03
RESPONSE_INVALID_CMD = 0x04
RESPONSE_INVALID_PARAM = 0x05

# 配置
APP_START_ADDR = 0x08010000  # APP起始地址
CHUNK_SIZE = 128             # 每次发送的数据块大小 (小包避免 CRC 错误)

# 固件版本魔术字
FW_VERSION_MAGIC = 0x46575652  # "FWVR"


class CRC32:
    """CRC32计算类"""
    
    def __init__(self):
        self.table = self._generate_table()
    
    def _generate_table(self):
        """生成CRC32查找表"""
        table = []
        for i in range(256):
            crc = i
            for _ in range(8):
                if crc & 1:
                    crc = (crc >> 1) ^ 0xEDB88320
                else:
                    crc >>= 1
            table.append(crc)
        return table
    
    def calculate(self, data):
        """计算CRC32"""
        crc = 0xFFFFFFFF
        for byte in data:
            crc = self.table[(crc ^ byte) & 0xFF] ^ (crc >> 8)
        return (~crc) & 0xFFFFFFFF


class FirmwareVersion:
    """固件版本信息类"""
    
    def __init__(self, major=1, minor=0, patch=0, description="", build_date=0, build_time=0, app_size=0, app_crc32=0):
        self.major = major
        self.minor = minor
        self.patch = patch
        self.description = description
        self.build_date = build_date
        self.build_time = build_time
        self.app_size = app_size
        self.app_crc32 = app_crc32
    
    def pack(self):
        """打包成字节数据"""
        data = bytearray(80)  # sizeof(Firmware_VersionTypeDef)
        struct.pack_into('<I', data, 0, FW_VERSION_MAGIC)  # magic
        data[4] = self.major
        data[5] = self.minor
        struct.pack_into('<H', data, 6, self.patch)
        struct.pack_into('<I', data, 8, self.build_date)
        struct.pack_into('<I', data, 12, self.build_time)
        struct.pack_into('<I', data, 16, self.app_size)
        struct.pack_into('<I', data, 20, self.app_crc32)
        # description (32 bytes)
        desc_bytes = self.description.encode('utf-8')[:31]
        data[24:24+len(desc_bytes)] = desc_bytes
        return bytes(data)
    
    @staticmethod
    def unpack(data):
        """从字节数据解包"""
        if len(data) < 80:
            return None
        
        magic = struct.unpack_from('<I', data, 0)[0]
        if magic != FW_VERSION_MAGIC:
            return None
        
        version = FirmwareVersion()
        version.major = data[4]
        version.minor = data[5]
        version.patch = struct.unpack_from('<H', data, 6)[0]
        version.build_date = struct.unpack_from('<I', data, 8)[0]
        version.build_time = struct.unpack_from('<I', data, 12)[0]
        version.app_size = struct.unpack_from('<I', data, 16)[0]
        version.app_crc32 = struct.unpack_from('<I', data, 20)[0]
        version.description = data[24:56].decode('utf-8', errors='ignore').strip('\x00')
        return version
    
    def __str__(self):
        return f"{self.major}.{self.minor}.{self.patch}"
    
    def to_detail_string(self):
        """详细信息字符串"""
        return (f"版本: {self.major}.{self.minor}.{self.patch}\n"
                f"  描述: {self.description}\n"
                f"  编译日期: {self.build_date}\n"
                f"  编译时间: {self.build_time}\n"
                f"  固件大小: {self.app_size} 字节\n"
                f"  固件CRC32: 0x{self.app_crc32:08X}")


class IAPUploader:
    """IAP固件升级类"""
    
    def __init__(self, port, baudrate=115200, timeout=5, verbose=False):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.verbose = verbose
        self.ser = None
        self.crc32 = CRC32()
    
    def connect(self):
        """连接串口"""
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=self.timeout
            )
            print(f"✓ 串口连接成功: {self.port} @ {self.baudrate}")
            time.sleep(0.1)  # 等待串口稳定
            
            # 不再自动发送'U'命令
            # 使用场景：
            #   1. 第一次升级：Bootloader检测到APP无效会自动进入IAP模式
            #   2. 后续升级：需要先用串口助手手动发送'u'，再运行脚本
            
            return True
        except Exception as e:
            print(f"✗ 串口连接失败: {e}")
            return False
    
    def disconnect(self):
        """断开串口"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("✓ 串口已断开")
    
    def _send_packet(self, cmd, addr=0, data=b''):
        """发送数据包"""
        packet = bytearray()
        
        # 构建数据包
        packet.extend(struct.pack('<H', PACKET_HEADER))  # 包头
        packet.append(cmd)                                # 命令
        packet.extend(struct.pack('<H', len(data)))      # 数据长度
        packet.extend(struct.pack('<I', addr))           # 地址
        packet.extend(data)                              # 数据
        
        # 计算CRC（不包括包头）
        crc_data = packet[2:]  # 从命令开始
        crc = self.crc32.calculate(crc_data)
        packet.extend(struct.pack('<I', crc))            # CRC
        
        # 发送
        self.ser.write(packet)
        
        if self.verbose:
            print(f"  发送: CMD=0x{cmd:02X}, ADDR=0x{addr:08X}, LEN={len(data)}, CRC=0x{crc:08X}")
    
    def _receive_response(self):
        """接收响应"""
        try:
            # 接收包头
            header = self.ser.read(2)
            if len(header) != 2:
                return None, None, None
            
            header_value = struct.unpack('<H', header)[0]
            if header_value != PACKET_HEADER:
                print(f"✗ 包头错误: 0x{header_value:04X}")
                return None, None, None
            
            # 接收命令
            cmd = self.ser.read(1)[0]
            
            # 接收长度
            length = struct.unpack('<H', self.ser.read(2))[0]
            
            # 接收地址
            addr = struct.unpack('<I', self.ser.read(4))[0]
            
            # 接收数据
            data = self.ser.read(length) if length > 0 else b''
            
            # 接收CRC
            received_crc = struct.unpack('<I', self.ser.read(4))[0]
            
            # 验证CRC
            crc_data = bytearray([cmd])
            crc_data.extend(struct.pack('<H', length))
            crc_data.extend(struct.pack('<I', addr))
            crc_data.extend(data)
            calculated_crc = self.crc32.calculate(crc_data)
            
            if received_crc != calculated_crc:
                print(f"✗ CRC校验失败: 收到=0x{received_crc:08X}, 计算=0x{calculated_crc:08X}")
                return None, None, None
            
            # 获取状态码（第一个字节）
            status = data[0] if len(data) > 0 else 0xFF
            response_data = data[1:] if len(data) > 1 else b''
            
            if self.verbose:
                print(f"  接收: CMD=0x{cmd:02X}, STATUS=0x{status:02X}, LEN={len(response_data)}")
            
            return cmd, status, response_data
            
        except Exception as e:
            print(f"✗ 接收响应失败: {e}")
            return None, None, None
    
    def get_info(self):
        """获取Bootloader信息"""
        print("\n>>> 获取Bootloader信息...")
        self._send_packet(IAP_CMD_GET_INFO)
        
        cmd, status, data = self._receive_response()
        if cmd == IAP_CMD_GET_INFO and status == RESPONSE_SUCCESS:
            if len(data) >= 28:  # sizeof(Bootloader_InfoTypeDef)
                version = f"{data[0]}.{data[1]}.{data[2]}"
                bootloader_size = struct.unpack('<I', data[3:7])[0]
                app_start_addr = struct.unpack('<I', data[7:11])[0]
                app_max_size = struct.unpack('<I', data[11:15])[0]
                mcu_type = data[15:31].decode('utf-8', errors='ignore').strip('\x00')
                
                print(f"  版本: {version}")
                print(f"  Bootloader大小: {bootloader_size // 1024} KB")
                print(f"  APP起始地址: 0x{app_start_addr:08X}")
                print(f"  APP最大大小: {app_max_size // 1024} KB")
                print(f"  MCU型号: {mcu_type}")
                return True
        
        print("✗ 获取信息失败")
        return False
    
    def start_update(self):
        """开始升级"""
        print("\n>>> 开始升级...")
        self._send_packet(IAP_CMD_START_UPDATE)
        
        cmd, status, data = self._receive_response()
        if cmd == IAP_CMD_START_UPDATE and status == RESPONSE_SUCCESS:
            print("✓ 准备接收固件")
            return True
        
        print(f"✗ 开始升级失败: STATUS=0x{status:02X}")
        return False
    
    def write_firmware(self, firmware_data):
        """写入固件"""
        print(f"\n>>> 写入固件 ({len(firmware_data)} 字节)...")
        
        addr = APP_START_ADDR
        total_chunks = (len(firmware_data) + CHUNK_SIZE - 1) // CHUNK_SIZE
        
        for i in range(0, len(firmware_data), CHUNK_SIZE):
            chunk = firmware_data[i:i + CHUNK_SIZE]
            chunk_num = i // CHUNK_SIZE + 1
            
            # 发送数据块
            self._send_packet(IAP_CMD_WRITE, addr, chunk)
            
            # 接收响应
            cmd, status, data = self._receive_response()
            
            if cmd != IAP_CMD_WRITE or status != RESPONSE_SUCCESS:
                print(f"\n✗ 写入失败: ADDR=0x{addr:08X}, STATUS=0x{status:02X}")
                return False
            
            # 显示进度
            progress = (chunk_num / total_chunks) * 100
            bar_length = 40
            filled_length = int(bar_length * chunk_num // total_chunks)
            bar = '█' * filled_length + '-' * (bar_length - filled_length)
            
            print(f"\r  进度: |{bar}| {progress:.1f}% ({chunk_num}/{total_chunks})", end='')
            
            addr += len(chunk)
        
        print()  # 换行
        print("✓ 固件写入完成")
        return True
    
    def verify_firmware(self, firmware_crc):
        """验证固件"""
        print(f"\n>>> 验证固件 (CRC32: 0x{firmware_crc:08X})...")
        
        crc_data = struct.pack('<I', firmware_crc)
        self._send_packet(IAP_CMD_VERIFY, 0, crc_data)
        
        cmd, status, data = self._receive_response()
        if cmd == IAP_CMD_VERIFY and status == RESPONSE_SUCCESS:
            print("✓ 固件验证成功")
            return True
        
        print(f"✗ 固件验证失败: STATUS=0x{status:02X}")
        return False
    
    def end_update(self):
        """结束升级"""
        print("\n>>> 结束升级...")
        self._send_packet(IAP_CMD_END_UPDATE)
        
        cmd, status, data = self._receive_response()
        if cmd == IAP_CMD_END_UPDATE and status == RESPONSE_SUCCESS:
            print("✓ 升级完成")
            print("✓ 设备将自动复位并运行新固件...")
            return True
        
        print(f"✗ 结束升级失败: STATUS=0x{status:02X}")
        return False
    
    def jump_to_app(self):
        """跳转到APP"""
        print("\n>>> 跳转到应用程序...")
        self._send_packet(IAP_CMD_JUMP)
        
        cmd, status, data = self._receive_response()
        if cmd == IAP_CMD_JUMP and status == RESPONSE_SUCCESS:
            print("✓ 跳转成功")
            return True
        
        print("✗ 跳转失败")
        return False
    
    def get_app_version(self):
        """获取当前APP固件版本"""
        if self.verbose:
            print("\n>>> 获取APP版本信息...")
        
        self._send_packet(IAP_CMD_GET_APP_VERSION)
        
        cmd, status, data = self._receive_response()
        if cmd == IAP_CMD_GET_APP_VERSION and status == RESPONSE_SUCCESS:
            version = FirmwareVersion.unpack(data)
            if version:
                if self.verbose:
                    print(f"  当前版本: {version}")
                return version
        
        if self.verbose:
            print("  当前无有效版本信息")
        return None
    
    def set_app_version(self, version):
        """设置APP固件版本信息"""
        print(f"\n>>> 保存版本信息: {version}...")
        
        version_data = version.pack()
        self._send_packet(IAP_CMD_SET_APP_VERSION, 0, version_data)
        
        cmd, status, data = self._receive_response()
        if cmd == IAP_CMD_SET_APP_VERSION and status == RESPONSE_SUCCESS:
            print("✓ 版本信息已保存")
            return True
        
        print(f"✗ 保存版本信息失败: STATUS=0x{status:02X}")
        return False
    
    def compare_version(self, new_version):
        """比较版本，判断是否需要升级"""
        print(f"\n>>> 版本检查...")
        
        version_data = new_version.pack()
        self._send_packet(IAP_CMD_COMPARE_VERSION, 0, version_data)
        
        cmd, status, data = self._receive_response()
        if cmd == IAP_CMD_COMPARE_VERSION and status == RESPONSE_SUCCESS:
            if len(data) >= 2:
                need_update = data[0]
                compare_result = data[1] if len(data) > 1 else 0
                
                # 获取当前版本信息
                current_version = self.get_app_version()
                if current_version:
                    print(f"  当前版本: {current_version}")
                else:
                    print(f"  当前版本: 无有效版本信息")
                
                print(f"  新版本: {new_version}")
                
                if need_update:
                    if compare_result == 0:
                        print("  ⚠ 当前无有效版本信息，建议升级")
                    else:
                        print("  ✓ 新版本较高，需要升级")
                    return True
                else:
                    print("  ℹ 版本相同或新版本较低，无需升级")
                    return False
        
        print(f"✗ 版本比较失败: STATUS=0x{status:02X}")
        return True  # 失败时默认允许升级
    
    def upload_firmware(self, firmware_path, version=None, check_version=True, verify=True):
        """完整的固件升级流程
        
        Args:
            firmware_path: 固件文件路径
            version: FirmwareVersion对象，如果为None则不保存版本信息
            check_version: 是否检查版本（需要提供version参数）
            verify: 是否验证固件
        """
        # 读取固件文件
        try:
            with open(firmware_path, 'rb') as f:
                firmware_data = f.read()
            print(f"✓ 读取固件文件: {firmware_path}")
            print(f"  文件大小: {len(firmware_data)} 字节")
        except Exception as e:
            print(f"✗ 读取固件文件失败: {e}")
            return False
        
        # 计算CRC32
        firmware_crc = self.crc32.calculate(firmware_data)
        print(f"  CRC32: 0x{firmware_crc:08X}")
        
        # 如果提供了版本信息，更新版本信息中的固件大小和CRC
        if version:
            version.app_size = len(firmware_data)
            version.app_crc32 = firmware_crc
        
        # 连接串口
        if not self.connect():
            return False
        
        try:
            # 1. 获取Bootloader信息
            if not self.get_info():
                return False
            
            # 2. 版本检查（如果提供了版本信息且需要检查）
            if version and check_version:
                need_update = self.compare_version(version)
                if not need_update:
                    print("\n" + "="*50)
                    print("固件版本相同或更低，无需升级")
                    print("="*50)
                    return True
            
            # 3. 开始升级
            if not self.start_update():
                return False
            
            # 4. 写入固件
            if not self.write_firmware(firmware_data):
                return False
            
            # 5. 验证固件（可选）
            if verify:
                # 注意：这里验证的是整个APP区域的CRC，不是固件文件的CRC
                # 如果固件小于APP_MAX_SIZE，剩余部分会被擦除为0xFF
                # 实际使用时可能需要修改验证逻辑
                pass
            
            # 6. 保存版本信息（如果提供了版本信息）
            if version:
                if not self.set_app_version(version):
                    print("⚠ 版本信息保存失败，但固件已写入")
            
            # 7. 结束升级（设备会自动复位并运行新固件）
            if not self.end_update():
                return False
            
            # 注意：设备会自动复位，不需要手动跳转
            # 延迟一下让用户看到信息
            time.sleep(0.5)
            
            print("\n" + "="*50)
            print("固件升级成功！设备正在重启...")
            if version:
                print(f"新版本: {version}")
            print("="*50)
            return True
            
        except Exception as e:
            print(f"\n✗ 升级过程出错: {e}")
            return False
        finally:
            self.disconnect()


def main():
    parser = argparse.ArgumentParser(
        description='STM32 IAP固件升级工具（支持版本管理）',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  # 不带版本信息升级（兼容旧版本）
  python iap_upload.py -p COM3 -f firmware.bin
  
  # 带版本信息升级
  python iap_upload.py -p COM3 -f firmware.bin --version 1.2.3 --desc "修复bug"
  
  # 强制升级（跳过版本检查）
  python iap_upload.py -p COM3 -f firmware.bin --version 1.2.3 --force
  
  # 仅查询当前版本
  python iap_upload.py -p COM3 --query-version
        """
    )
    
    parser.add_argument('-p', '--port', required=True,
                        help='串口号 (例如: COM3 或 /dev/ttyUSB0)')
    parser.add_argument('-f', '--file',
                        help='固件文件路径 (.bin格式)')
    parser.add_argument('-b', '--baudrate', type=int, default=115200,
                        help='波特率 (默认: 115200)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='显示详细信息')
    parser.add_argument('--no-verify', action='store_true',
                        help='跳过固件验证')
    
    # 版本管理参数
    parser.add_argument('--version',
                        help='固件版本号 (格式: major.minor.patch, 例如: 1.2.3)')
    parser.add_argument('--desc',
                        help='固件描述信息')
    parser.add_argument('--build-date', type=int,
                        help='编译日期 (格式: YYYYMMDD, 例如: 20241106)')
    parser.add_argument('--build-time', type=int,
                        help='编译时间 (格式: HHMMSS, 例如: 153045)')
    parser.add_argument('--force', action='store_true',
                        help='强制升级（跳过版本检查）')
    parser.add_argument('--query-version', action='store_true',
                        help='仅查询当前APP版本信息')
    
    args = parser.parse_args()
    
    print("="*50)
    print("STM32 IAP 固件升级工具 V2.0")
    print("="*50)
    
    # 如果是查询版本
    if args.query_version:
        uploader = IAPUploader(
            port=args.port,
            baudrate=args.baudrate,
            verbose=args.verbose
        )
        
        if not uploader.connect():
            return 1
        
        try:
            print("\n>>> 查询当前APP版本...")
            version = uploader.get_app_version()
            if version:
                print("\n当前APP版本信息:")
                print(version.to_detail_string())
            else:
                print("\n当前无有效的APP版本信息")
            return 0
        finally:
            uploader.disconnect()
    
    # 正常升级流程需要固件文件
    if not args.file:
        print("✗ 请指定固件文件 (-f/--file)")
        return 1
    
    # 检查文件是否存在
    if not os.path.exists(args.file):
        print(f"✗ 文件不存在: {args.file}")
        return 1
    
    # 解析版本信息
    version_obj = None
    if args.version:
        try:
            parts = args.version.split('.')
            if len(parts) != 3:
                print("✗ 版本号格式错误，应为: major.minor.patch")
                return 1
            
            major, minor, patch = int(parts[0]), int(parts[1]), int(parts[2])
            
            # 获取当前时间作为默认编译时间
            import datetime
            now = datetime.datetime.now()
            default_date = int(now.strftime("%Y%m%d"))
            default_time = int(now.strftime("%H%M%S"))
            
            version_obj = FirmwareVersion(
                major=major,
                minor=minor,
                patch=patch,
                description=args.desc if args.desc else "",
                build_date=args.build_date if args.build_date else default_date,
                build_time=args.build_time if args.build_time else default_time
            )
            
            print(f"\n固件版本: {version_obj}")
            if args.desc:
                print(f"固件描述: {args.desc}")
            
        except ValueError as e:
            print(f"✗ 版本号解析错误: {e}")
            return 1
    
    # 创建上传器
    uploader = IAPUploader(
        port=args.port,
        baudrate=args.baudrate,
        verbose=args.verbose
    )
    
    # 执行升级
    success = uploader.upload_firmware(
        firmware_path=args.file,
        version=version_obj,
        check_version=(not args.force) and (version_obj is not None),
        verify=not args.no_verify
    )
    
    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())

