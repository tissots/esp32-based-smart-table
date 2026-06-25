
# <p align="center"><font color="#00d2d3">Smart Table v3.0</font></p>

<p align="center">
  <a href="https://platformio.org/"><img src="https://img.shields.io/badge/PlatformIO-FF6B00?style=for-the-badge&logo=platformio&logoColor=white" alt="PlatformIO"></a>
  <a href="https://www.espressif.com/"><img src="https://img.shields.io/badge/ESP32-000000?style=for-the-badge&logo=espressif&logoColor=white" alt="ESP32"></a>
  <a href="https://opensource.org/licenses/MIT"><img src="https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge" alt="License: MIT"></a>
</p>

<p align="center"><strong>An ESP32-based automation system that controls a fan, lights, and two extra relays based on temperature, motion, ambient light, and time-of-day — all configurable over Bluetooth without any WiFi dependency.</strong></p>

<p align="center">Built around FreeRTOS with proper task separation, mutex-protected shared state, NVS persistence, and a soft real-time clock that survives reboots.</p>

---

## <font color="#10ac84">What it does</font>

The system sits on a table (or wherever you mount it) and makes decisions automatically:

* **Fan** turns on when the temperature crosses a threshold during a configured time window
* **Lights** turn on when it's dark, there's been continuous motion for a while, and you're within the active hours
* **R2 and R3** are manual relays you can toggle via button or Bluetooth
* Everything can be overridden manually via Bluetooth commands or the physical buttons

![image alt](https://github.com/tissots/esp32-based-smart-table/blob/main/IMG_20260519_134951_208.jpg?raw=true)

The goal was to avoid false triggers — the kind where a PIR sensor twitches for two seconds and the lights flash on at 2am. So there's proper debouncing: the system requires sustained motion and sustained darkness before it acts.

---

## <font color="#2e86de">Hardware Configuration</font>

### Pin Assignment

| Component | Pin | Notes |
| :--- | :---: | :--- |
| **Relay 1 (Fan)** | GPIO 25 | Active LOW |
| **Relay 2** | GPIO 26 | Active LOW |
| **Relay 3** | GPIO 27 | Active LOW |
| **Relay 4 (Lights)** | GPIO 14 | Active LOW |
| **Buzzer** | GPIO 16 | LEDC channel 0, 2kHz |
| **Buttons (ADC ladder)** | GPIO 33 | 5 buttons on a voltage divider |
| **PIR sensor** | GPIO 17 | Digital input |
| **LDR** | GPIO 35 | Analog input |
| **DHT11** | GPIO 4 | Temperature + humidity |

> **Hardware Note:** The 5 buttons share a single ADC pin via a resistor ladder. Each button produces a different voltage level — the code maps ADC ranges to button numbers.

![image alt](https://github.com/tissots/esp32-based-smart-table/blob/main/IMG_20260517_104016_249.jpg?raw=true)

---

## <font color="#ff9f43">Software Dependencies</font>

* Arduino framework (ESP32)
* [DHT sensor library](https://github.com/adafruit/DHT-sensor-library) — for DHT11
* FreeRTOS (bundled with ESP32 Arduino core)
* BluetoothSerial (bundled with ESP32 Arduino core)
* Preferences (NVS — bundled with ESP32 Arduino core)

---

## <font color="#ee5253">How to build</font>

This project uses PlatformIO. If you do not have it, install the VSCode extension.

1. **Clone or copy the project**
2. **Open in VSCode with PlatformIO**
3. **Verify Configuration:** Make sure your `platformio.ini` targets the ESP32 (e.g. `espressif32` platform, `esp32dev` board)
4. **Deploy:** Upload and open the Serial Monitor at 115200 baud

---

![image alt](https://github.com/tissots/esp32-based-smart-table/blob/main/IMG_20260517_104001_886.jpg?raw=true)

## <font color="#9b59b6">Task structure</font>

The firmware splits processing duties across FreeRTOS tasks to maintain responsiveness:

| Task | Core | Priority | Stack | What it does |
| :--- | :---: | :---: | :---: | :--- |
| `sensorTask` | 1 | 2 | 4096 | Reads DHT11, PIR, LDR — runs debouncing logic, ticks the soft clock |
| `logicTask` | 1 | 2 | 4096 | Makes fan/lights decisions based on sensor state + time windows |
| `buttonTask` | 1 | 1 | 3072 | Reads ADC button ladder, dispatches commands |
| `buzzerTask` | 1 | 1 | 2048 | Handles buzzer timing (beep duration) |
| `btTask` | 0 | 1 | 8192 | Bluetooth command parser + live status stream |

> **Concurrency Protection:** Shared state is protected by a single FreeRTOS mutex (`stateMutex`). Tasks take the mutex, update state, and release it. The watchdog timer (10 second timeout) is registered on all tasks.

---

## <font color="#00d2d3">Time handling</font>

There is no NTP or hardware RTC connection. Instead, the firmware uses a tri-tier fallback architecture:

1. **Soft clock:** Increments in software using `millis()`. Drifts a little over time but works fine for time-window logic.
2. **RTC memory:** Stores the last known time in `RTC_DATA_ATTR`. Survives deep sleep and soft resets.
3. **NVS backup:** Saves the time to flash periodically. Survives power loss but will be behind by however long the device was powered down.

On boot, it tries RTC memory first, then falls back to NVS. If neither has data, the clock starts at 00:00:00 until configured over Bluetooth.

To set the time manually:

```
TIME 14:35:00
```

---

## <font color="#54a0ff">Bluetooth commands</font>

Connect to `SmartTable` over Bluetooth Serial using any terminal application (such as Serial Bluetooth Terminal on Android).

### Basic control

| Command | Effect |
| :--- | :--- |
| `STATUS` | Full system status dump |
| `ON` | Enable master switch |
| `OFF` | Disable master switch (turns everything off) |
| `TEMP` | Current temperature, humidity, and threshold |
| `GETTIME` | Current time and sync status |
| `TIME HH:MM:SS` | Set the clock |

### Fan control

| Command | Effect |
| :--- | :--- |
| `FAN AUTO` | Temperature-based automatic control |
| `FAN ON` | Force fan on |
| `FAN OFF` | Force fan off |

### Lights control

| Command | Effect |
| :--- | :--- |
| `LIGHT AUTO` | PIR + LDR + time window based control |
| `LIGHT ON` | Force lights on |
| `LIGHT OFF` | Force lights off |

### Relay control

| Command | Effect |
| :--- | :--- |
| `R2 ON` / `R2 OFF` | Toggle relay 2 |
| `R3 ON` / `R3 OFF` | Toggle relay 3 |

### Settings

| Command | Effect |
| :--- | :--- |
| `SET TEMP <value>` | Set fan temperature threshold (e.g. `SET TEMP 26.5`) |
| `SET LIGHTTIME HH:MM-HH:MM` | Set lights active window |
| `SET FANTIME HH:MM-HH:MM` | Set fan active window |
| `SAVE` | Manually save settings to NVS |
| `LOAD` | Reload settings from NVS |
| `CLEAR` | Wipe NVS settings (resets to defaults on next boot) |
| `REBOOT` | Save and restart |
| `HELP` | List all commands |

While connected, the device automatically streams a live status line every 5 seconds.

---

## <font color="#5f27cd">Physical buttons</font>

The five hardware buttons on the ADC ladder map to the following functionality:

| Button | Function |
| :---: | :--- |
| **1** | Toggle master ON/OFF |
| **2** | Toggle R2 |
| **3** | Toggle R3 |
| **4** | Cycle fan mode (AUTO -> MANUAL ON -> MANUAL OFF -> AUTO) |
| **5** | Toggle lights mode (AUTO <-> MANUAL ON) |

> **Debounce Policy:** Button presses carry a 2-second software lockout to prevent accidental double-triggers.

---

## <font color="#ff6b6b">Auto lights logic</font>

The automation logic is intentionally conservative to eliminate false triggers. For lights to turn on automatically, all of the following conditions must be met simultaneously:

1. The current clock time falls inside the configured time window.
2. The LDR reading has registered "dark" continuously for at least **10 seconds**.
3. The PIR has detected motion continuously for at least **10 seconds** (with a 3-second grace period allowed for brief drops).
4. A final 2-second safety buffer passes after motion stability is confirmed.

If motion disappears for more than **3 seconds**, the motion timer resets completely. This prevents an individual simply walking past from staging a countdown.

### Deactivation Rules
Lights will turn off when any of the following occur:
* No motion has been detected for **5 minutes**.
* The active time window ends (backed by a 30-second buffer to prevent sudden blackouts mid-activity).
* Ambient light returns AND motion has ceased for over 1 minute.

---

## <font color="#ff9f43">Fan auto logic</font>

The fan system utilizes a simpler, state-driven approach. The fan turns on when:
* The system is inside the fan time window.
* Temperature exceeds `fanThreshold + 0.5°C` (hysteresis upper bound).

The fan turns off when:
* Temperature drops below `fanThreshold - 0.5°C` (hysteresis lower bound).
* The execution system leaves the time window.

The 0.5°C hysteresis margin prevents rapid relay toggling when ambient temperature rests directly on the threshold.

---

## <font color="#10ac84">Temperature calibration</font>

The DHT11 sensor exhibits a known high bias. A global offset of **-5.0°C** is applied to raw inputs. Modify `TEMP_OFFSET` in the source file to match your hardware profile.

```cpp
// Readings are smoothed with an exponential moving average 
// (30% new reading, 70% previous value) to clear electrical noise.

```
## <font color="#576574">Configurable constants</font>
These variables are located at the top of the main source file for quick edits without using the terminal interface:
```cpp
#define LDR_DARK_THRESHOLD    10        // ADC below this = dark
#define DARK_DEBOUNCE_TIME    10000UL   // 10s continuous darkness required
#define MOTION_DEBOUNCE_TIME  10000UL   // 10s continuous motion required
#define MOTION_GRACE_PERIOD   3000UL    // 3s grace before motion timer resets
#define LIGHTS_OFF_DELAY      300000UL  // 5 min no motion -> lights off
#define TIME_WINDOW_EXIT_BUFFER 30000UL // 30s buffer when leaving time window
#define LIGHTS_ON_SAFETY_BUFFER 2000UL  // Extra 2s after motion stability
const float TEMP_OFFSET       = -5.0;   // DHT11 calibration

```
### Factory Defaults
 * **Lights Window:** 18:30 – 05:30
 * **Fan Window:** 18:30 – 02:00
 * **Fan Threshold:** 25.0°C
## <font color="#222f3e">Persistence</font>
Runtime modifications automatically save to NVS (Non-Volatile Storage) under the namespace smarttable.
Saved parameters include:
 * Master power state
 * Fan mode and target temperature threshold
 * Lights operational mode
 * Light and fan active time windows
 * Last known time (used for emergency recovery upon rebooting)
## <font color="#8395a7">Known limitations</font>
 * **No Network Sync:** Lacks WiFi/NTP capabilities; time drifts naturally and requires re-syncing after extended power outages.
 * **Sensor Speed:** The DHT11 is restricted to one sample per 2 seconds and carries a wide accuracy variance.
 * **Clock Drift:** The soft clock drifts roughly ±a few seconds per operational hour.
 * **Bluetooth Constraints:** Uses Classic Bluetooth; iOS devices do not connect natively without BLE support.
## <font color="#576574">License</font>
This project is licensed under unrestrictive terms — feel free to modify, deploy, and distribute. Code improvements regarding debouncing routines or NTP expansion are welcome
