"""
SmartTable Data Dashboard
Charts and graphs from your saved data
"""

import sqlite3
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from datetime import datetime, timedelta

DB_FILE = "smarttable.db"

def load_data(hours=24):
    """Load data from database"""
    conn = sqlite3.connect(DB_FILE)
    
    query = f"""
        SELECT timestamp, temp, hum, motion, dark, fan_state, light_state
        FROM readings
        WHERE timestamp > datetime('now', '-{hours} hours')
        ORDER BY timestamp
    """
    
    df = pd.read_sql_query(query, conn)
    conn.close()
    
    if df.empty:
        print(f"No data found for the last {hours} hours.")
        return None
    
    df['timestamp'] = pd.to_datetime(df['timestamp'])
    return df


def show_temperature_chart(df):
    """Temperature over time"""
    plt.figure(figsize=(12, 4))
    plt.plot(df['timestamp'], df['temp'], color='red', linewidth=1.5)
    plt.axhline(y=25.0, color='orange', linestyle='--', alpha=0.5, label='Fan Threshold (25°C)')
    plt.fill_between(df['timestamp'], df['temp'], alpha=0.2, color='red')
    plt.title('Temperature Over Time')
    plt.xlabel('Time')
    plt.ylabel('Temperature (°C)')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.xticks(rotation=45)
    plt.tight_layout()
    plt.show()


def show_humidity_chart(df):
    """Humidity over time"""
    plt.figure(figsize=(12, 4))
    plt.plot(df['timestamp'], df['hum'], color='blue', linewidth=1.5)
    plt.fill_between(df['timestamp'], df['hum'], alpha=0.2, color='blue')
    plt.title('Humidity Over Time')
    plt.xlabel('Time')
    plt.ylabel('Humidity (%)')
    plt.grid(True, alpha=0.3)
    plt.xticks(rotation=45)
    plt.tight_layout()
    plt.show()


def show_motion_chart(df):
    """Motion events over time"""
    plt.figure(figsize=(12, 3))
    
    # Plot motion as vertical bars
    motion_times = df[df['motion'] == 1]['timestamp']
    for t in motion_times:
        plt.axvline(x=t, color='green', alpha=0.3, linewidth=2)
    
    # Plot dark periods
    dark_times = df[df['dark'] == 1]['timestamp']
    for t in dark_times:
        plt.axvline(x=t, color='gray', alpha=0.2, linewidth=2)
    
    from matplotlib.patches import Patch
    legend_elements = [
        Patch(facecolor='green', alpha=0.3, label='Motion Detected'),
        Patch(facecolor='gray', alpha=0.2, label='Dark')
    ]
    
    plt.title('Motion & Darkness Events')
    plt.xlabel('Time')
    plt.legend(handles=legend_elements)
    plt.grid(True, alpha=0.3)
    plt.xticks(rotation=45)
    plt.tight_layout()
    plt.show()


def show_relay_chart(df):
    """Fan and Lights state over time"""
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 6), sharex=True)
    
    # Fan state
    ax1.step(df['timestamp'], df['fan_state'], where='post', color='orange', linewidth=2)
    ax1.set_ylabel('Fan')
    ax1.set_yticks([0, 1])
    ax1.set_yticklabels(['OFF', 'ON'])
    ax1.grid(True, alpha=0.3)
    ax1.set_title('Fan & Lights State')
    
    # Lights state
    ax2.step(df['timestamp'], df['light_state'], where='post', color='yellow', linewidth=2)
    ax2.set_ylabel('Lights')
    ax2.set_yticks([0, 1])
    ax2.set_yticklabels(['OFF', 'ON'])
    ax2.set_xlabel('Time')
    ax2.grid(True, alpha=0.3)
    
    plt.xticks(rotation=45)
    plt.tight_layout()
    plt.show()


def show_summary(df):
    """Summary statistics"""
    print("\n" + "=" * 50)
    print("  DATA SUMMARY")
    print("=" * 50)
    print(f"  Period: {df['timestamp'].min()} to {df['timestamp'].max()}")
    print(f"  Total readings: {len(df)}")
    print(f"  Temperature: {df['temp'].mean():.1f}°C (min: {df['temp'].min():.1f}°C, max: {df['temp'].max():.1f}°C)")
    print(f"  Humidity: {df['hum'].mean():.0f}% (min: {df['hum'].min():.0f}%, max: {df['hum'].max():.0f}%)")
    print(f"  Motion detected: {df['motion'].sum()} times ({(df['motion'].sum()/len(df)*100):.0f}%)")
    print(f"  Fan was ON: {df['fan_state'].sum()} times ({(df['fan_state'].sum()/len(df)*100):.0f}%)")
    print(f"  Lights were ON: {df['light_state'].sum()} times ({(df['light_state'].sum()/len(df)*100):.0f}%)")
    print("=" * 50)


def show_all_charts(hours=24):
    """Show all charts"""
    df = load_data(hours)
    
    if df is None:
        return
    
    show_summary(df)
    show_temperature_chart(df)
    show_humidity_chart(df)
    show_motion_chart(df)
    show_relay_chart(df)


def export_to_csv():
    """Export all data to CSV for Excel"""
    conn = sqlite3.connect(DB_FILE)
    df = pd.read_sql_query("SELECT * FROM readings ORDER BY timestamp", conn)
    conn.close()
    
    filename = f"smarttable_data_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
    df.to_csv(filename, index=False)
    print(f"[EXPORT] Saved {len(df)} rows to {filename}")


# ─────────────────────────────────────────
#  MENU
# ─────────────────────────────────────────

if __name__ == "__main__":
    print("=" * 50)
    print("  SmartTable Dashboard")
    print("=" * 50)
    print("\nOptions:")
    print("  1. Show last 1 hour")
    print("  2. Show last 6 hours")
    print("  3. Show last 24 hours")
    print("  4. Show last 7 days")
    print("  5. Export to CSV")
    print("  6. Temperature only")
    print("  7. Motion only")
    print("  8. Relay states only")
    print("  9. Exit")
    
    choice = input("\nChoice: ").strip()
    
    if choice == '1':
        show_all_charts(1)
    elif choice == '2':
        show_all_charts(6)
    elif choice == '3':
        show_all_charts(24)
    elif choice == '4':
        show_all_charts(168)
    elif choice == '5':
        export_to_csv()
    elif choice == '6':
        df = load_data(24)
        if df is not None:
            show_temperature_chart(df)
    elif choice == '7':
        df = load_data(24)
        if df is not None:
            show_motion_chart(df)
    elif choice == '8':
        df = load_data(24)
        if df is not None:
            show_relay_chart(df)