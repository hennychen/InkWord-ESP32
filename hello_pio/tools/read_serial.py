import serial, time, sys
ser = serial.Serial('/dev/cu.usbserial-0001', 115200, timeout=1)
ser.dtr = False
ser.rts = False
t0 = time.time()
while time.time() - t0 < 25:
    line = ser.readline()
    if line:
        sys.stdout.write(line.decode('utf-8', 'replace'))
        sys.stdout.flush()
ser.close()
