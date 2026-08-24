#!/usr/bin/env python3
"""真机验收串口监控（临时脚本，验收完删除）。

用法: python3 .serial_watch.py <输出日志> <时长秒>
RTS 脉冲复位设备以捕获完整启动日志（同 esptool 硬复位路径）。
"""
import sys
import time

import serial

PORT = "/dev/cu.usbserial-0001"
BAUD = 115200

out_path = sys.argv[1] if len(sys.argv) > 1 else "serial.log"
duration = int(sys.argv[2]) if len(sys.argv) > 2 else 600

ser = serial.Serial(PORT, BAUD, timeout=1)
# RTS 脉冲复位（esptool 同款），复位后松开正常运行
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
ser.reset_input_buffer()

end = time.time() + duration
with open(out_path, "ab") as f:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            f.write(data)
            f.flush()
ser.close()
