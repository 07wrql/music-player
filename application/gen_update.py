#!/usr/bin/env python3
"""
VS1003B 固件生成工具
用法: 把 Keil 编译出的 vs1003b.hex 拖到这个脚本上, 自动生成 UPDATE.bin
      或者双击运行 (自动在同目录下找 vs1003b.hex)
"""
import sys, os

def hex_to_bin(hex_path, bin_path):
    """Intel HEX -> 原始二进制 (从 APP1_BASE 开始)"""
    data = {}
    base_addr = 0
    min_addr = 0xFFFFFFFF
    max_addr = 0

    with open(hex_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line[0] != ':':
                continue
            byte_count = int(line[1:3], 16)
            address = int(line[3:7], 16)
            record_type = int(line[7:9], 16)

            if record_type == 0x00:
                addr = base_addr + address
                for i in range(byte_count):
                    data[addr + i] = int(line[9+2*i:11+2*i], 16)
                min_addr = min(min_addr, addr)
                max_addr = max(max_addr, addr + byte_count)
            elif record_type == 0x04:
                base_addr = int(line[9:13], 16) << 16
            elif record_type == 0x01:
                break

    APP1_BASE = 0x08010000
    if min_addr != APP1_BASE:
        print(f'警告: 固件起始地址 0x{min_addr:08X}, 预期 0x{APP1_BASE:08X}')

    size = max_addr - APP1_BASE
    buf = bytearray(size)
    for addr in range(APP1_BASE, max_addr):
        buf[addr - APP1_BASE] = data.get(addr, 0xFF)

    with open(bin_path, 'wb') as f:
        f.write(buf)

    return size


if __name__ == '__main__':
    # 1. 找 hex 文件
    if len(sys.argv) > 1:
        hex_file = sys.argv[1]
    else:
        # 默认路径: Keil 编译输出目录
        script_dir = os.path.dirname(os.path.abspath(__file__))
        default = os.path.join(script_dir,
            r'MDK-ARM\vs1003b\vs1003b.hex')
        if os.path.exists(default):
            hex_file = default
        else:
            print('用法: 将 vs1003b.hex 拖到此脚本上')
            print(f'或把脚本放到项目根目录 (已尝试: {default})')
            input('按回车退出...')
            sys.exit(1)

    if not os.path.exists(hex_file):
        print(f'错误: 找不到 {hex_file}')
        input('按回车退出...')
        sys.exit(1)

    # 2. 输出到同目录
    out_dir = os.path.dirname(os.path.abspath(hex_file))
    bin_file = os.path.join(out_dir, 'UPDATE.bin')

    # 3. 转换
    print(f'输入: {hex_file}')
    size = hex_to_bin(hex_file, bin_file)
    print(f'输出: {bin_file}')
    print(f'大小: {size} 字节 ({size/1024:.1f} KB)')
    print()
    print('完成! 将 UPDATE.bin 复制到 SD 卡根目录即可升级。')
    input('按回车退出...')
