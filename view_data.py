"""Quick data viewer"""
import sqlite3

conn = sqlite3.connect("smarttable.db")

# Today's summary
print("\n=== TODAY'S SUMMARY ===")
row = conn.execute("""
    SELECT COUNT(*), AVG(temp), MIN(temp), MAX(temp), AVG(hum)
    FROM readings WHERE date(timestamp)=date('now')
""").fetchone()
print(f"Readings: {row[0]} | Temp: {row[1]:.1f}°C (min:{row[2]:.1f} max:{row[3]:.1f}) | Hum: {row[4]:.0f}%")

# Last reading
row = conn.execute("SELECT * FROM readings ORDER BY timestamp DESC LIMIT 1").fetchone()
if row:
    print(f"\nLast: {row[0]} | T:{row[1]}°C H:{row[2]}% PIR:{'YES' if row[3] else 'NO'} Fan:{'ON' if row[5] else 'OFF'} Light:{'ON' if row[6] else 'OFF'}")

conn.close()