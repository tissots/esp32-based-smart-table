"""
SmartTable Bluetooth Data Logger
COM8 - 9600 baud
"""

import serial
import sqlite3
import datetime
import re
import time

# ─────────────────────────────────────────
#  SETUP
# ─────────────────────────────────────────
DB_FILE = "smarttable.db"
bt = serial.Serial("COM8", 9600, timeout=1)

# Create database
conn = sqlite3.connect(DB_FILE)
conn.execute('''CREATE TABLE IF NOT EXISTS readings (
    timestamp TEXT, temp REAL, hum REAL, motion INTEGER, 
    dark INTEGER, fan_state INTEGER, light_state INTEGER)''')
conn.commit()
conn.close()

# Auto-sync time
now = datetime.datetime.now()
bt.write(f"TIME {now.hour:02d}:{now.minute:02d}:{now.second:02d}\n".encode())
print(f"[TIME] Sent: {now.strftime('%H:%M:%S')}")

time.sleep(1)
bt.reset_input_buffer()

print(f"\n[LOGGER] Connected to COM8. Ctrl+C to stop.\n")

# ─────────────────────────────────────────
#  MAIN LOOP
# ─────────────────────────────────────────
try:
    while True:
        if bt.in_waiting:
            line = bt.readline().decode('utf-8', errors='ignore').strip()
            
            if '[LIVE]' in line:
                # Parse LIVE data
                temp = re.search(r'T:(\d+\.?\d*)C', line)
                hum = re.search(r'H:(\d+\.?\d*)%', line)
                pir = re.search(r'PIR:(YES|NO)', line)
                dark = re.search(r'Dark:(YES|NO)', line)
                fan = re.search(r'Fan:(ON|OFF)', line)
                light = re.search(r'Light:(ON|OFF)', line)
                
                t_val = float(temp.group(1)) if temp else 0
                h_val = float(hum.group(1)) if hum else 0
                p_val = 1 if pir and pir.group(1) == 'YES' else 0
                d_val = 1 if dark and dark.group(1) == 'YES' else 0
                f_val = 1 if fan and fan.group(1) == 'ON' else 0
                l_val = 1 if light and light.group(1) == 'ON' else 0
                
                # Save to database
                conn = sqlite3.connect(DB_FILE)
                conn.execute('INSERT INTO readings VALUES (?,?,?,?,?,?,?)',
                    (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                     t_val, h_val, p_val, d_val, f_val, l_val))
                conn.commit()
                conn.close()
                
                # Print
                print(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] "
                      f"T:{t_val}°C H:{h_val}% "
                      f"PIR:{'YES' if p_val else 'NO'} "
                      f"Fan:{'ON' if f_val else 'OFF'} "
                      f"Light:{'ON' if l_val else 'OFF'}")

except KeyboardInterrupt:
    print("\n[LOGGER] Stopped")
    bt.close()