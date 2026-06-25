"""
SmartTable Voice Control
Speak commands to control your table
"""

import serial
import speech_recognition as sr
import pyttsx3
import datetime
import re
import threading
import time

# ─────────────────────────────────────────
#  SETUP
# ─────────────────────────────────────────
bt = serial.Serial("COM8", 9600, timeout=1)
engine = pyttsx3.init()
recognizer = sr.Recognizer()

# Auto-sync time
now = datetime.datetime.now()
bt.write(f"TIME {now.hour:02d}:{now.minute:02d}:{now.second:02d}\n".encode())
time.sleep(0.5)
bt.reset_input_buffer()

# ─────────────────────────────────────────
#  VOICE COMMANDS MAPPING
# ─────────────────────────────────────────
COMMANDS = {
    # Lights
    "turn on lights": "LIGHT ON",
    "turn off lights": "LIGHT OFF",
    "lights on": "LIGHT ON",
    "lights off": "LIGHT OFF",
    "lights auto": "LIGHT AUTO",
    
    # Fan
    "turn on fan": "FAN ON",
    "turn off fan": "FAN OFF",
    "fan on": "FAN ON",
    "fan off": "FAN OFF",
    "fan auto": "FAN AUTO",
    
    # Master
    "master on": "ON",
    "master off": "OFF",
    "turn on table": "ON",
    "turn off table": "OFF",
    
    # Relays
    "r2 on": "R2 ON",
    "r2 off": "R2 OFF",
    "r3 on": "R3 ON",
    "r3 off": "R3 OFF",
    
    # Info
    "status": "STATUS",
    "temperature": "TEMP",
    "what is the temperature": "TEMP",
    "how hot is it": "TEMP",
    "goodnight": "OFF",
    "good night": "OFF",
    "goodbye": "OFF",
}


def speak(text):
    """Text to speech"""
    print(f"[SPEAK] {text}")
    engine.say(text)
    engine.runAndWait()


def send_command(cmd):
    """Send command to ESP32"""
    print(f"[SEND] {cmd}")
    bt.write((cmd + "\n").encode())
    time.sleep(0.3)
    response = ""
    while bt.in_waiting:
        response += bt.readline().decode('utf-8', errors='ignore')
    return response.strip()


def get_temperature():
    """Get temperature and speak it"""
    response = send_command("TEMP")
    # Parse: "Temperature: 25.1C | Humidity: 65% | Threshold: 25.0C"
    temp_match = re.search(r'Temperature: (\d+\.?\d*)C', response)
    hum_match = re.search(r'Humidity: (\d+\.?\d*)%', response)
    
    if temp_match:
        temp = temp_match.group(1)
        hum = hum_match.group(1) if hum_match else "unknown"
        return f"Temperature is {temp} degrees Celsius, humidity is {hum} percent"
    return "Could not get temperature"


def listen_for_command():
    """Listen for voice command"""
    with sr.Microphone() as source:
        print("\n[LISTENING] Speak...")
        recognizer.adjust_for_ambient_noise(source, duration=0.5)
        
        try:
            audio = recognizer.listen(source, timeout=5, phrase_time_limit=3)
            text = recognizer.recognize_google(audio).lower()
            print(f"[HEARD] {text}")
            return text
        except sr.WaitTimeoutError:
            return None
        except sr.UnknownValueError:
            print("[HEARD] (not understood)")
            return None
        except sr.RequestError:
            print("[ERROR] Speech recognition service unavailable")
            return None


def process_command(text):
    """Match voice text to command"""
    text = text.lower().strip()
    
    # Check exact matches
    for voice_cmd, bt_cmd in COMMANDS.items():
        if voice_cmd in text:
            return bt_cmd
    
    return None


# ─────────────────────────────────────────
#  MAIN LOOP
# ─────────────────────────────────────────
print("=" * 50)
print("  SmartTable Voice Control")
print("=" * 50)
speak("Smart Table voice control ready")

try:
    while True:
        command = listen_for_command()
        
        if command is None:
            continue
        
        if command in ['exit', 'quit', 'stop']:
            speak("Goodbye")
            break
        
        # Check for temperature query
        if any(word in command for word in ['temperature', 'temp', 'hot', 'warm', 'cold', 'humid']):
            response = get_temperature()
            speak(response)
            continue
        
        # Process command
        bt_cmd = process_command(command)
        
        if bt_cmd:
            response = send_command(bt_cmd)
            if response:
                print(f"[RESPONSE] {response}")
            speak("Done")
        else:
            print(f"[UNKNOWN] Command not recognized: {command}")

except KeyboardInterrupt:
    print("\n[VOICE] Stopped")

bt.close()