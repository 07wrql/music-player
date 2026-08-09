#!/usr/bin/env python3
"""
PC 端固件发送工具 — 通过 YModem 协议发送 .bin 固件到 STM32

依赖: pip install pyserial

用法:
    python ymodem_send.py COM3 firmware_v1.1.bin

原理:
    YModem 协议:
    1. 等待接收方发送 'C' (CRC模式请求)
    2. 发送 Packet 0: 文件名 + 文件大小
    3. 发送 Packet 1..N: 1024字节数据块 (CRC16校验)
    4. 发送 EOT (End of Transmission)
    5. 发送空 Packet 0 结束批次

作者: IAP Upgrade Tool
"""

import serial
import struct
import sys
import time
import os
import argparse


# ======== YModem 协议常量 ========
SOH = 0x01          # 128-byte packet (仅 packet 0 使用)
STX = 0x02          # 1024-byte packet
EOT = 0x04          # End of Transmission
ACK = 0x06          # Acknowledge
NAK = 0x15          # Negative Acknowledge
CAN = 0x18          # Cancel
C   = 0x43          # 'C' — CRC mode request

PACKET_HEADER_SIZE = 3   # SOH/STX + block_num + ~block_num
PACKET_DATA_SIZE   = 1024
PACKET_CRC_SIZE    = 2


def crc16(data: bytes) -> int:
    """CRC16-CCITT (polynomial 0x1021)"""
    crc = 0
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = (crc << 1) ^ 0x1021
            else:
                crc = (crc << 1)
        crc &= 0xFFFF
    return crc


def send_packet(ser: serial.Serial, pkt_type: int, block_num: int,
                data: bytes, data_len: int) -> bool:
    """发送一个 YModem 数据包"""
    assert len(data) >= data_len

    # 填充不足部分为 0x1A (CPMEOF)
    padded = bytearray(data)
    if data_len < PACKET_DATA_SIZE:
        padded[data_len:] = b'\x1A' * (PACKET_DATA_SIZE - data_len)

    # 构造包: [SOH/STX] [block_num] [~block_num] [data...] [CRC_high] [CRC_low]
    pkt = bytearray()
    pkt.append(pkt_type)
    pkt.append(block_num & 0xFF)
    pkt.append((~block_num) & 0xFF)
    pkt.extend(padded[:data_len])

    crc = crc16(bytes(padded[:data_len]))  # 注意: 只对 data_len 范围计算 CRC
    pkt.append((crc >> 8) & 0xFF)
    pkt.append(crc & 0xFF)

    ser.write(bytes(pkt))
    ser.flush()
    return True


def wait_for_byte(ser: serial.Serial, expected: bytes, timeout: float = 5.0) -> bool:
    """等待接收指定字节"""
    ser.timeout = timeout
    try:
        ch = ser.read(1)
        if ch in expected:
            return True
        print(f"  [!] 期望 {expected.hex()}, 收到 {ch.hex() if ch else 'TIMEOUT'}")
    except:
        pass
    return False


def ymodem_send(port: str, filepath: str, baudrate: int = 115200):
    """
    YModem 发送主流程
    """
    # 检查文件
    if not os.path.isfile(filepath):
        print(f"[ERROR] 文件不存在: {filepath}")
        sys.exit(1)

    filename = os.path.basename(filepath)
    fw_size  = os.path.getsize(filepath)

    print(f"[INFO] 固件文件: {filename}")
    print(f"[INFO] 固件大小: {fw_size} bytes ({fw_size / 1024:.1f} KB)")
    print(f"[INFO] 串口: {port} @ {baudrate} baud")

    # 打开串口
    ser = serial.Serial(port, baudrate, timeout=1)
    print(f"[INFO] 串口已打开, 等待接收方请求...")

    # ========== 阶段 1: 等待 'C' 请求 ==========
    print("[PHASE 1] 等待接收方发送 'C' (CRC模式请求)...")
    if not wait_for_byte(ser, b'C', timeout=30.0):
        print("[ERROR] 超时: 未收到 'C' 请求")
        ser.close()
        sys.exit(1)
    print("[PHASE 1] 收到 'C' → 准备发送文件头")

    # ========== 阶段 2: 发送 Packet 0 (文件名 + 大小) ==========
    print("[PHASE 2] 发送 Packet 0 (文件头)...")

    # 文件头格式: "filename\x00size\n..." (共128字节, 用 \x00 填充)
    header_info = f"{filename}\x00{fw_size}\n".encode()
    header_pkt = bytearray(128)
    header_pkt[:len(header_info)] = header_info

    send_packet(ser, SOH, 0, bytes(header_pkt), 128)

    # 等待 ACK
    if not wait_for_byte(ser, b'\x06', timeout=5.0):
        print("[ERROR] Packet 0 未收到 ACK")
        ser.close()
        sys.exit(1)
    print("[PHASE 2] Packet 0 ACK")

    # 等待 'C' (下一个包的请求)
    if not wait_for_byte(ser, b'C', timeout=5.0):
        print("[WARN] 未收到第二次 'C', 继续发送数据包...")

    # ========== 阶段 3: 发送数据包 ==========
    print("[PHASE 3] 开始发送数据包...")

    with open(filepath, "rb") as f:
        fw_data = f.read()

    total_packets = (fw_size + PACKET_DATA_SIZE - 1) // PACKET_DATA_SIZE
    block_num = 1

    for offset in range(0, fw_size, PACKET_DATA_SIZE):
        chunk = fw_data[offset:offset + PACKET_DATA_SIZE]
        chunk_len = len(chunk)

        # 发送 STX (1024-byte) 或 SOH (128-byte, 仅当剩余数据 ≤ 128)
        if chunk_len <= 128:
            pkt_type = SOH
            data_len = 128
        else:
            pkt_type = STX
            data_len = PACKET_DATA_SIZE

        padded = bytearray(data_len)
        padded[:chunk_len] = chunk[:chunk_len]

        send_packet(ser, pkt_type, block_num, bytes(padded), data_len)

        # 等待 ACK
        if not wait_for_byte(ser, b'\x06', timeout=5.0):
            print(f"\n[ERROR] Packet {block_num} 未收到 ACK")
            ser.close()
            sys.exit(1)

        progress = (offset + chunk_len) * 100 // fw_size
        print(f"\r  进度: {progress:3d}%  (Packet {block_num}/{total_packets})", end="")
        sys.stdout.flush()

        block_num = (block_num + 1) & 0xFF

    print(f"\n[PHASE 3] 数据发送完成 ({total_packets} 个包)")

    # ========== 阶段 4: 发送 EOT ==========
    print("[PHASE 4] 发送 EOT...")
    ser.write(bytes([EOT]))
    ser.flush()

    # 等待 NAK (接收方要求第二个 EOT)
    if wait_for_byte(ser, b'\x15', timeout=5.0):
        print("  收到 NAK → 发送第二个 EOT")
    else:
        print("  未收到 NAK (可能接收方直接 ACK 了)")

    ser.write(bytes([EOT]))
    ser.flush()

    # 等待 ACK
    if wait_for_byte(ser, b'\x06', timeout=5.0):
        print("  收到 ACK: EOT 成功")

    # ========== 阶段 5: 结束批次 ==========
    print("[PHASE 5] 发送空包结束批次...")
    empty_header = bytearray(128)  # 全 0x00 = 空文件名 = 批次结束
    send_packet(ser, SOH, 0, bytes(empty_header), 128)

    if wait_for_byte(ser, b'\x06', timeout=5.0):
        print("  收到 ACK: 批次结束")

    ser.close()
    print("\n[DONE] 固件发送完成! STM32 正在重启...")


def main():
    parser = argparse.ArgumentParser(
        description="YModem 固件发送工具 (STM32 IAP)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python ymodem_send.py COM3 firmware.bin
  python ymodem_send.py /dev/ttyUSB0 firmware.bin -b 115200
        """)

    parser.add_argument("port",     help="串口号 (如 COM3, /dev/ttyUSB0)")
    parser.add_argument("file",     help="固件文件 (.bin)")
    parser.add_argument("-b", "--baudrate", type=int, default=115200,
                        help="波特率 (默认: 115200)")

    args = parser.parse_args()
    ymodem_send(args.port, args.file, args.baudrate)


if __name__ == "__main__":
    main()
