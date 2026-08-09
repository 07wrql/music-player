#!/usr/bin/env python3
"""
PC → STM32 音频文件发送工具 (YModem 协议)

用法:
    python send_file.py COM3 song.mp3

步骤:
    1. 脚本通过串口发送 "#SEND#" 指令
    2. 设备响应 'C' (YModem CRC 模式)
    3. 脚本通过 YModem 协议发送文件
    4. 设备写文件到 SD 卡, 完成后发 "DONE"
"""

import serial
import sys
import os
import struct
import time

PACKET_SIZE = 1024
SOH = 0x01
STX = 0x02
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
C   = 0x43


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = (crc << 1) ^ 0x1021 if crc & 0x8000 else crc << 1
    return crc & 0xFFFF


def send_ymodem(ser, filepath):
    """ 通过 YModem 协议发送文件 """
    fname = os.path.basename(filepath)
    fsize = os.path.getsize(filepath)

    # 等待接收方发 'C'
    print("等待设备就绪 ('C')...")
    for _ in range(60):  # 60 次 × 100ms = 6 秒超时
        if ser.in_waiting:
            ch = ser.read(1)[0]
            if ch == C:
                print("设备就绪, 开始传输")
                break
            elif ch == NAK:
                pass  # 继续等
        time.sleep(0.1)
    else:
        print("错误: 设备未就绪")
        return False

    # Packet 0: 文件头 (128 字节)
    header = fname.encode() + b'\x00' + str(fsize).encode() + b'\x00'
    header = header[:128].ljust(128, b'\x00')

    packet = bytes([SOH, 0x00, 0xFF]) + header
    packet += struct.pack('>H', crc16(header))
    ser.write(packet)
    ser.flush()

    # 等 ACK + 'C'
    for _ in range(100):
        if ser.in_waiting:
            ch = ser.read(1)[0]
            if ch == ACK:
                if ser.in_waiting:
                    ch2 = ser.read(1)[0]
                    if ch2 == C:
                        break
            elif ch == CAN:
                print("设备取消了传输")
                return False
        time.sleep(0.1)
    else:
        print("错误: 设备未响应 packet 0")
        return False

    # 发送数据包
    with open(filepath, 'rb') as f:
        blk_num = 1
        while True:
            data = f.read(PACKET_SIZE)
            if not data:
                break

            actual_len = len(data)
            data = data.ljust(PACKET_SIZE, b'\x1A')

            pkt_type = STX if actual_len == PACKET_SIZE else SOH
            blk = bytes([pkt_type, blk_num & 0xFF,
                        (~blk_num) & 0xFF]) + data
            blk += struct.pack('>H', crc16(data))
            ser.write(blk)
            ser.flush()

            # 等 ACK
            for _ in range(100):
                if ser.in_waiting:
                    ch = ser.read(1)[0]
                    if ch == ACK:
                        break
                    elif ch == CAN:
                        print("设备取消了传输")
                        return False
                time.sleep(0.1)
            else:
                print(f"错误: block {blk_num} 无响应")
                return False

            blk_num += 1
            pct = min(100, int(blk_num * PACKET_SIZE / fsize * 100))
            print(f"\r进度: {pct}%", end='', flush=True)

    # 结束传输
    ser.write(bytes([EOT]))
    ser.flush()
    time.sleep(0.5)

    # 等第二次 EOT 确认
    for _ in range(50):
        if ser.in_waiting:
            ch = ser.read(1)[0]
            if ch == NAK:
                ser.write(bytes([EOT]))
                ser.flush()
            elif ch == ACK:
                print("\n传输完成!")
                return True
        time.sleep(0.1)

    print("\n警告: 未收到最终确认")
    return True


def main():
    if len(sys.argv) < 3:
        print(f"用法: {sys.argv[0]} <串口> <文件路径>")
        print(f"示例: {sys.argv[0]} COM3 song.mp3")
        sys.exit(1)

    port = sys.argv[1]
    filepath = sys.argv[2]

    if not os.path.exists(filepath):
        print(f"错误: 文件不存在: {filepath}")
        sys.exit(1)

    fsize = os.path.getsize(filepath)
    print(f"文件: {os.path.basename(filepath)} ({fsize:,} 字节)")
    print(f"串口: {port}")

    ser = serial.Serial(port, 115200, timeout=1)
    print(f"打开串口 {port} @115200")

    # 发送触发指令
    print("发送 #SEND# 指令...")
    ser.write(b"#SEND#\r\n")
    ser.flush()

    success = send_ymodem(ser, filepath)

    # 读取设备回复
    time.sleep(0.5)
    while ser.in_waiting:
        print("设备:", ser.readline().decode(errors='replace').strip())

    ser.close()
    if success:
        print("文件发送成功!")
    else:
        print("文件发送失败!")
        sys.exit(1)


if __name__ == '__main__':
    main()
