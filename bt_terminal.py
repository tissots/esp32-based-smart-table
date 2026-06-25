"""
SmartTable Bluetooth Terminal
COM8 - Send commands manually
"""

import serial
import datetime

bt = serial.Serial("COM8", 9600, timeout=1)

# Auto-sync time
now = datetime.datetime.now()
bt.write(f"TIME {now.hour:02d}:{now.minute:02d}:{now.second:02d}\n".encode())
print(f"[TIME] Synced: {now.strftime('%H:%M:%S')}")

print("\n=== SmartTable Terminal ===")
print("Type commands: STATUS, ON, OFF, FAN ON, FAN OFF, LIGHT ON, etc.")
print("Type 'exit' to quit\n")

try:
    while True:
        # Check for incoming data from ESP32
        if bt.in_waiting:
            print(bt.readline().decode('utf-8', errors='ignore').strip())
        
        # Get user input
        cmd = input("> ").strip()
        
        if cmd.lower() == 'exit':
            break
        
        if cmd:
            bt.write((cmd + "\n").encode())

except KeyboardInterrupt:
    pass

bt.close()
print("\n[BT] Disconnected")