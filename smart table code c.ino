#include <Arduino.h>
#include "DHT.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include "BluetoothSerial.h"
#include <Preferences.h>

// Forward declarations
void initSoftClock(int h, int m, int s);

// ─────────────────────────────────────────
//  PIN DEFINITIONS
// ─────────────────────────────────────────
#define R1_PIN         25
#define R2_PIN         26
#define R3_PIN         27
#define R4_PIN         14
#define BUZZER_PIN     16
#define BUZZER_CHANNEL 0
#define BUZZER_FREQ    2000
#define BUZZER_RES     8
#define BUTTON_ADC_PIN 33
#define PIR_PIN        17
#define LDR_PIN        35
#define DHTPIN         4

// ─────────────────────────────────────────
//  WATCHDOG
// ─────────────────────────────────────────
#define WDT_TIMEOUT_SEC 10

// ─────────────────────────────────────────
//  BLUETOOTH
// ─────────────────────────────────────────
#define BT_DEVICE_NAME  "SmartTable"
BluetoothSerial SerialBT;

// ─────────────────────────────────────────
//  NVS PREFERENCES
// ─────────────────────────────────────────
Preferences prefs;

// ─────────────────────────────────────────
//  RTC MEMORY FOR TIME PERSISTENCE
// ─────────────────────────────────────────
RTC_DATA_ATTR struct RTCData {
  uint32_t magic;         // Magic number to validate data
  int hour;
  int minute;
  int second;
} rtcData;

#define RTC_MAGIC 0x55AA55AA

// ─────────────────────────────────────────
//  DHT11 - CALIBRATED VERSION
// ─────────────────────────────────────────
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ─────────────────────────────────────────
//  MUTEX
// ─────────────────────────────────────────
SemaphoreHandle_t stateMutex = NULL;

// ─────────────────────────────────────────
//  SHARED STATE (volatile for multi-task access)
// ─────────────────────────────────────────
volatile float calibratedTemp  = 0.0;
volatile float humidity        = 0.0;
volatile bool  motionDetected  = false;
volatile bool  isDark          = false;
volatile int   ldrValue        = 0;
volatile bool  darkStable      = false;
volatile unsigned long darkStartTime = 0;

enum RelayMode { AUTO, MANUAL };
enum FanMode   { FAN_AUTO, FAN_MANUAL_ON, FAN_MANUAL_OFF };

volatile FanMode   fanMode     = FAN_AUTO;
volatile bool      fanState    = false;
volatile RelayMode lightsMode  = AUTO;
volatile bool      lightsState = false;
volatile bool      r2State     = false;
volatile bool      r3State     = false;

// Motion tracking for lights control
volatile unsigned long motionStartTime = 0;     // When motion first detected
volatile bool motionStable = false;             // Motion stable for debounce time
volatile unsigned long lastMotionTime = 0;      // Last time motion was detected
volatile unsigned long lightsTurnedOnAt = 0;
volatile bool  lightsAutoOn     = false;
volatile unsigned long motionLostTime = 0;      // When motion was last lost
volatile bool motionRecentlyLost = false;       // Flag for recent motion loss

volatile bool          buzzerActive    = false;
volatile unsigned long buzzerStartTime = 0;

struct TimeNow { int hour; int minute; int second; };
TimeNow       currentTime     = {0, 0, 0};
volatile bool timeSynced      = false;
unsigned long lastClockUpdate = 0;

volatile bool masterEnabled  = true;

// Configurable settings (saved to NVS)
volatile int lightStartHour = 18;
volatile int lightStartMin  = 30;
volatile int lightEndHour   = 5;
volatile int lightEndMin    = 30;
volatile int fanStartHour   = 18;
volatile int fanStartMin    = 30;
volatile int fanEndHour     = 2;
volatile int fanEndMin      = 0;
volatile float fanThreshold = 25.0;

// ─────────────────────────────────────────
//  CALIBRATION & TIMING CONSTANTS
// ─────────────────────────────────────────
const float TEMP_OFFSET   = -5.0;         // DHT11 calibration offset

#define LDR_DARK_THRESHOLD  10            // ADC value below this = dark
#define DARK_DEBOUNCE_TIME  10000UL       // 10 seconds of continuous darkness
#define MOTION_DEBOUNCE_TIME 10000UL      // 10 seconds of continuous motion
#define MOTION_GRACE_PERIOD 3000UL        // 3 seconds grace for brief motion loss
#define LIGHTS_OFF_DELAY    300000UL      // 5 minutes of NO motion before auto-off
#define TIME_WINDOW_EXIT_BUFFER 30000UL   // 30 seconds buffer when leaving time window
#define LIGHTS_ON_SAFETY_BUFFER 2000UL    // 2 seconds extra after stability confirmed
#define DHT_INTERVAL        2000          // DHT reading interval (ms)

// Temperature hysteresis to prevent rapid toggling
#define TEMP_HYSTERESIS     0.5

// Track actual relay output states to prevent redundant writes
volatile bool actualFanOn    = false;
volatile bool actualLightsOn = false;
volatile bool actualR2On     = false;
volatile bool actualR3On     = false;

// ─────────────────────────────────────────
//  NVS SAVE & LOAD FUNCTIONS
// ─────────────────────────────────────────

void saveSettings() {
  prefs.begin("smarttable", false);
  
  prefs.putBool("master", masterEnabled);
  prefs.putInt("fanMode", (int)fanMode);
  prefs.putInt("lightMode", (int)lightsMode);
  prefs.putFloat("fanThresh", fanThreshold);
  
  prefs.putInt("lsHour", lightStartHour);
  prefs.putInt("lsMin", lightStartMin);
  prefs.putInt("leHour", lightEndHour);
  prefs.putInt("leMin", lightEndMin);
  
  prefs.putInt("fsHour", fanStartHour);
  prefs.putInt("fsMin", fanStartMin);
  prefs.putInt("feHour", fanEndHour);
  prefs.putInt("feMin", fanEndMin);
  
  prefs.end();
  
  Serial.println("[NVS] Settings saved to flash");
}

void loadSettings() {
  prefs.begin("smarttable", true);
  
  if (prefs.isKey("master")) {
    masterEnabled = prefs.getBool("master", true);
    fanMode = (FanMode)prefs.getInt("fanMode", FAN_AUTO);
    lightsMode = (RelayMode)prefs.getInt("lightMode", AUTO);
    fanThreshold = prefs.getFloat("fanThresh", 25.0);
    
    lightStartHour = prefs.getInt("lsHour", 18);
    lightStartMin = prefs.getInt("lsMin", 30);
    lightEndHour = prefs.getInt("leHour", 5);
    lightEndMin = prefs.getInt("leMin", 30);
    
    fanStartHour = prefs.getInt("fsHour", 18);
    fanStartMin = prefs.getInt("fsMin", 30);
    fanEndHour = prefs.getInt("feHour", 2);
    fanEndMin = prefs.getInt("feMin", 0);
    
    Serial.println("[NVS] Settings loaded from flash");
    Serial.printf("[NVS] Master: %s, FanThreshold: %.1fC\n", 
                  masterEnabled ? "ON" : "OFF", fanThreshold);
  } else {
    Serial.println("[NVS] First boot - saving defaults");
    prefs.end();
    saveSettings();
  }
  
  prefs.end();
}

void clearSettings() {
  prefs.begin("smarttable", false);
  prefs.clear();
  prefs.end();
  Serial.println("[NVS] All settings cleared");
}

// ─────────────────────────────────────────
//  RTC TIME SAVE & RESTORE FUNCTIONS
// ─────────────────────────────────────────

void saveTimeToRTC() {
  rtcData.magic = RTC_MAGIC;
  rtcData.hour = currentTime.hour;
  rtcData.minute = currentTime.minute;
  rtcData.second = currentTime.second;
  
  Serial.printf("[RTC] Time saved: %02d:%02d:%02d\n", 
                rtcData.hour, rtcData.minute, rtcData.second);
}

void saveTimeToNVS() {
  prefs.begin("smarttable", false);
  prefs.putInt("rtcHour", currentTime.hour);
  prefs.putInt("rtcMin", currentTime.minute);
  prefs.putInt("rtcSec", currentTime.second);
  prefs.end();
  Serial.printf("[NVS] Time backup saved: %02d:%02d:%02d\n", 
                currentTime.hour, currentTime.minute, currentTime.second);
}

bool restoreTimeFromMemory() {
  // Step 1: Try RTC memory first (survives deep sleep and resets)
  if (rtcData.magic == RTC_MAGIC) {
    initSoftClock(rtcData.hour, rtcData.minute, rtcData.second);
    timeSynced = true;
    Serial.printf("[RTC] Time restored from RTC memory: %02d:%02d:%02d\n",
                  rtcData.hour, rtcData.minute, rtcData.second);
    return true;
  }
  
  // Step 2: Fall back to NVS (survives power loss but drifts)
  prefs.begin("smarttable", true);
  if (prefs.isKey("rtcHour")) {
    int h = prefs.getInt("rtcHour", 0);
    int m = prefs.getInt("rtcMin", 0);
    int s = prefs.getInt("rtcSec", 0);
    prefs.end();
    
    if (h >= 0 && h <= 23 && m >= 0 && m <= 59 && s >= 0 && s <= 59) {
      initSoftClock(h, m, s);
      timeSynced = true;
      Serial.printf("[NVS] Time restored from NVS backup: %02d:%02d:%02d\n", h, m, s);
      Serial.println("[NVS] Note: Time may be behind due to power-off duration");
      return true;
    }
  } else {
    prefs.end();
  }
  
  Serial.println("[TIME] No saved time found - defaults to 00:00:00");
  return false;
}

// ─────────────────────────────────────────
//  RELAY HELPERS
// ─────────────────────────────────────────
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

const uint8_t relayPins[]  = {R1_PIN, R2_PIN, R3_PIN, R4_PIN};

void relayOn(uint8_t pin)  { digitalWrite(pin, RELAY_ON);  }
void relayOff(uint8_t pin) { digitalWrite(pin, RELAY_OFF); }

void allRelaysOff() {
  for (int i = 0; i < 4; i++) {
    relayOff(relayPins[i]);
  }
  actualFanOn = false;
  actualLightsOn = false;
  actualR2On = false;
  actualR3On = false;
}

// ─────────────────────────────────────────
//  BUZZER
// ─────────────────────────────────────────
void triggerBeep() {
  ledcWrite(BUZZER_CHANNEL, 128);
  buzzerStartTime = millis();
  buzzerActive    = true;
}

// ─────────────────────────────────────────
//  SOFT CLOCK
// ─────────────────────────────────────────
void initSoftClock(int h, int m, int s) {
  currentTime.hour   = h;
  currentTime.minute = m;
  currentTime.second = s;
  lastClockUpdate    = millis();
}

void tickSoftClock() {
  unsigned long now     = millis();
  unsigned long elapsed = now - lastClockUpdate;
  if (elapsed < 1000) return;

  unsigned long secs  = elapsed / 1000;
  lastClockUpdate    += secs * 1000;
  currentTime.second += secs;

  if (currentTime.second >= 60) {
    currentTime.minute += currentTime.second / 60;
    currentTime.second  = currentTime.second % 60;
  }
  if (currentTime.minute >= 60) {
    currentTime.hour  += currentTime.minute / 60;
    currentTime.minute = currentTime.minute % 60;
  }
  if (currentTime.hour >= 24) currentTime.hour = currentTime.hour % 24;
}

// ─────────────────────────────────────────
//  TIME WINDOW
// ─────────────────────────────────────────
bool inTimeWindow(int startH, int startM, int endH, int endM) {
  int now   = currentTime.hour * 60 + currentTime.minute;
  int start = startH * 60 + startM;
  int end   = endH   * 60 + endM;
  if (start <= end) return (now >= start && now <= end);
  return (now >= start || now <= end);
}

// ─────────────────────────────────────────
//  APPLY FUNCTIONS
// ─────────────────────────────────────────
void applyFan() {
  bool actualState = masterEnabled ? fanState : false;
  
  if (actualState != actualFanOn) {
    if (actualState) {
      relayOn(R1_PIN);
    } else {
      relayOff(R1_PIN);
    }
    actualFanOn = actualState;
    
    const char* m = (fanMode==FAN_AUTO)?"AUTO":(fanMode==FAN_MANUAL_ON)?"MANUAL-ON":"MANUAL-OFF";
    Serial.printf("[FAN] Mode: %s | State: %s\n", m, actualState ? "ON" : "OFF");
  }
}

void applyLights() {
  bool actualState = masterEnabled ? lightsState : false;
  
  if (actualState != actualLightsOn) {
    if (actualState) {
      relayOn(R4_PIN);
    } else {
      relayOff(R4_PIN);
    }
    actualLightsOn = actualState;
    
    Serial.printf("[LIGHTS] Mode: %s | State: %s\n",
                  lightsMode==MANUAL?"MANUAL":"AUTO", actualState?"ON":"OFF");
  }
}

void applyR2() {
  bool actualState = masterEnabled ? r2State : false;
  
  if (actualState != actualR2On) {
    if (actualState) {
      relayOn(R2_PIN);
    } else {
      relayOff(R2_PIN);
    }
    actualR2On = actualState;
    
    Serial.printf("[R2] State: %s\n", actualState ? "ON" : "OFF");
  }
}

void applyR3() {
  bool actualState = masterEnabled ? r3State : false;
  
  if (actualState != actualR3On) {
    if (actualState) {
      relayOn(R3_PIN);
    } else {
      relayOff(R3_PIN);
    }
    actualR3On = actualState;
    
    Serial.printf("[R3] State: %s\n", actualState ? "ON" : "OFF");
  }
}

void applyAll() {
  applyFan();
  applyLights();
  applyR2();
  applyR3();
}

// ─────────────────────────────────────────
//  STATUS STRING
// ─────────────────────────────────────────
String buildStatus() {
  String s = "\n===== SMART TABLE STATUS =====\n";
  s += "Master: ";         s += masterEnabled ? "ON\n" : "OFF\n";
  s += "Time: ";
  char tbuf[12];
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", 
           currentTime.hour, currentTime.minute, currentTime.second);
  s += tbuf;
  s += timeSynced ? " (synced)\n" : " (not synced)\n";
  s += "Temp: ";           s += calibratedTemp; s += "C (Threshold: "; s += fanThreshold; s += "C)\n";
  s += "Humidity: ";       s += humidity;        s += "%\n";
  s += "PIR: ";            s += motionDetected ? "MOTION\n" : "clear\n";
  s += "LDR: ";            s += ldrValue;
  s += " Dark: ";          s += isDark ? "YES" : "NO";
  s += " Stable: ";        s += darkStable ? "YES\n" : "NO\n";
  s += "Fan: ";
  s += (fanMode==FAN_AUTO)?"AUTO":((fanMode==FAN_MANUAL_ON)?"MANUAL-ON":"MANUAL-OFF");
  s += " | ";              s += fanState ? "ON\n" : "OFF\n";
  s += "Lights: ";         s += lightsMode==MANUAL?"MANUAL":"AUTO";
  s += " | ";              s += lightsState ? "ON\n" : "OFF\n";
  s += "R2: ";             s += r2State ? "ON\n" : "OFF\n";
  s += "R3: ";             s += r3State ? "ON\n" : "OFF\n";
  s += "Windows - Light: ";
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d-%02d:%02d", 
           lightStartHour, lightStartMin, lightEndHour, lightEndMin);
  s += tbuf;
  s += " Fan: ";
  snprintf(tbuf, sizeof(tbuf), "%02d:%02d-%02d:%02d\n", 
           fanStartHour, fanStartMin, fanEndHour, fanEndMin);
  s += tbuf;
  s += "BT Only (No WiFi)\n";
  s += "Persistent: YES (NVS+RTC)\n";
  s += "Timing: MotionDebounce=";
  s += MOTION_DEBOUNCE_TIME/1000;
  s += "s Grace=";
  s += MOTION_GRACE_PERIOD/1000;
  s += "s ExitBuffer=";
  s += TIME_WINDOW_EXIT_BUFFER/1000;
  s += "s\n";
  s += "==============================\n";
  return s;
}

// ═══════════════════════════════════════════════════════
//  FREERTOS TASKS
// ═══════════════════════════════════════════════════════

// ─────────────────────────────────────────
//  SENSOR TASK (Core 1)
// ─────────────────────────────────────────
void sensorTask(void* pvParameters) {
  esp_task_wdt_add(NULL);
  unsigned long lastDHTRead = 0;
  unsigned long lastPrint = 0;
  
  // For temperature smoothing
  float smoothedTemp = 0.0;
  float smoothedHum = 0.0;
  bool firstReading = true;

  for (;;) {
    esp_task_wdt_reset();
    unsigned long now = millis();
    
    bool  pir  = digitalRead(PIR_PIN);
    int   ldr  = analogRead(LDR_PIN);
    bool  dark = (ldr < LDR_DARK_THRESHOLD);

    float temp = calibratedTemp;
    float hum  = humidity;

    if (now - lastDHTRead >= DHT_INTERVAL) {
      float rawT = dht.readTemperature();
      float rawH = dht.readHumidity();
      
      // Validate readings
      if (!isnan(rawT) && !isnan(rawH) && rawT > -40 && rawT < 80 && rawH >= 0 && rawH <= 100) {
        temp = rawT + TEMP_OFFSET;
        hum  = rawH;
        
        // Apply exponential moving average for smoothing
        if (firstReading) {
          smoothedTemp = temp;
          smoothedHum = hum;
          firstReading = false;
        } else {
          smoothedTemp = (temp * 0.3) + (smoothedTemp * 0.7); // 30% new, 70% old
          smoothedHum = (hum * 0.3) + (smoothedHum * 0.7);
        }
        
        Serial.printf("[DHT] Raw: %.1f°C -> Calibrated: %.1f°C (Smoothed: %.1f°C)\n", 
                      rawT, temp, smoothedTemp);
      } else {
        Serial.println("[DHT] Read failed - keeping previous values");
        temp = smoothedTemp;
        hum = smoothedHum;
      }
      lastDHTRead = now;
    }

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      motionDetected = pir;
      ldrValue       = ldr;
      isDark         = dark;
      calibratedTemp = smoothedTemp;
      humidity       = smoothedHum;

      // ─────────────────────────────────────────
      //  FIXED: Motion debouncing - requires CONTINUOUS uninterrupted motion
      // ─────────────────────────────────────────
      if (pir) {
        // Motion detected
        if (motionStartTime == 0) {
          // First motion - start the timer
          motionStartTime = now;
          motionStable = false;
          motionRecentlyLost = false;
          motionLostTime = 0;
        }
        // Motion is present - clear any loss tracking
        motionLostTime = 0;
        motionRecentlyLost = false;
        lastMotionTime = now;
      } else {
        // No motion
        if (motionStartTime > 0) {
          // Motion was previously detected, now lost
          if (motionLostTime == 0) {
            // Just lost motion - mark the time
            motionLostTime = now;
            motionRecentlyLost = true;
            // RESET stability since motion was interrupted
            motionStable = false;
          }
          
          // If motion has been lost beyond grace period, RESET EVERYTHING
          if (motionLostTime > 0 && (now - motionLostTime) >= MOTION_GRACE_PERIOD) {
            motionStartTime = 0;
            motionStable = false;
            motionLostTime = 0;
            motionRecentlyLost = false;
          }
        }
      }

      // Dark debouncing
      if (isDark) {
        if (darkStartTime == 0) { 
          darkStartTime = now; 
          darkStable = false; 
        } else if (!darkStable && now - darkStartTime >= DARK_DEBOUNCE_TIME) {
          darkStable = true;
          Serial.println("[LDR] Darkness stable (10s continuous)");
        }
      } else {
        darkStartTime = 0;
        darkStable    = false;
      }

      tickSoftClock();
      xSemaphoreGive(stateMutex);
    }

    if (now - lastPrint >= 2000) {
      Serial.printf("[TIME] %02d:%02d:%02d %s | Temp:%.1fC | Hum:%.0f%% "
                    "| LDR:%d Dark:%s Stable:%s | PIR:%s Stable:%s\n",
                    currentTime.hour, currentTime.minute, currentTime.second,
                    timeSynced?"(sync)":"(unset)",
                    smoothedTemp, smoothedHum, ldr,
                    dark?"YES":"NO",
                    darkStable?"YES":"NO",
                    pir?"MOTION":"clear",
                    motionStable?"YES":"NO");
      lastPrint = now;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ─────────────────────────────────────────
//  LOGIC TASK - With proper timing controls
// ─────────────────────────────────────────
void logicTask(void* pvParameters) {
  esp_task_wdt_add(NULL);

  for (;;) {
    esp_task_wdt_reset();

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      unsigned long now = millis();
      
      bool inLightW = inTimeWindow(lightStartHour, lightStartMin, 
                                   lightEndHour, lightEndMin);
      bool inFanW   = inTimeWindow(fanStartHour, fanStartMin, 
                                   fanEndHour, fanEndMin);

      // Handle master OFF
      if (!masterEnabled) {
        if (fanState || lightsState || r2State || r3State) {
          fanState = false;
          lightsState = false;
          lightsAutoOn = false;
          r2State = false;
          r3State = false;
          applyFan();
          applyLights();
          applyR2();
          applyR3();
        }
        xSemaphoreGive(stateMutex);
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }

      // ============================================
      // FIXED: LIGHTS AUTO MODE - Requires CONTINUOUS motion
      // ============================================
      if (lightsMode == AUTO) {
        // TURN ON: Multiple conditions must ALL be met
        if (!lightsState) {
          // Check if motion has been stable for required time (continuous)
          if (motionStartTime > 0 && !motionStable && !motionRecentlyLost &&
              now - motionStartTime >= MOTION_DEBOUNCE_TIME) {
            motionStable = true;
            Serial.println("[PIR] Motion stable (20s continuous)");
          }
          
          // Only turn on if ALL conditions are met with extra safety buffer
          if (motionStable && darkStable && inLightW && !motionRecentlyLost &&
              now - motionStartTime >= MOTION_DEBOUNCE_TIME + LIGHTS_ON_SAFETY_BUFFER) {
            lightsState      = true;
            lightsAutoOn     = true;
            lightsTurnedOnAt = now;
            lastMotionTime   = now;
            applyLights();
            triggerBeep();
            Serial.printf("[LIGHTS] AUTO ON: Motion=%lums Dark=%s Window=%s (Safety buffer: %lums)\n",
                         now - motionStartTime, 
                         darkStable ? "YES" : "NO",
                         inLightW ? "YES" : "NO",
                         LIGHTS_ON_SAFETY_BUFFER);
          }
        }
        
        // Keep updating motion timer while motion is detected
        if (motionDetected && lightsAutoOn) {
          lastMotionTime = now;
          motionRecentlyLost = false;
          motionLostTime = 0;
        }
        
        // AUTO OFF LOGIC with time window exit buffer
        if (lightsAutoOn && lightsState) {
          bool shouldTurnOff = false;
          String reason = "";
          
          // Check 1: Outside time window with exit buffer
          if (!inLightW) {
            if (now - lightsTurnedOnAt > TIME_WINDOW_EXIT_BUFFER) {
              reason = "Outside time window (buffer expired)";
              shouldTurnOff = true;
            } else {
              Serial.printf("[LIGHTS] Outside window but in exit buffer (%lu/%lums remaining)\n",
                          TIME_WINDOW_EXIT_BUFFER - (now - lightsTurnedOnAt),
                          TIME_WINDOW_EXIT_BUFFER);
            }
          }
          // Check 2: NO motion for extended period
          else if (!motionDetected && (now - lastMotionTime >= LIGHTS_OFF_DELAY)) {
            reason = "No motion for 5 minutes";
            shouldTurnOff = true;
          }
          // Check 3: Not dark anymore (with debounce)
          else if (!darkStable && !motionDetected && 
                   now - lastMotionTime > 60000) {
            reason = "Light detected (not dark) + 1min no motion";
            shouldTurnOff = true;
          }
          
          if (shouldTurnOff) {
            lightsState  = false;
            lightsAutoOn = false;
            applyLights();
            Serial.printf("[LIGHTS] AUTO OFF: %s\n", reason.c_str());
          }
        }
      }

      // Handle Fan AUTO mode with hysteresis
      if (fanMode == FAN_AUTO) {
        if (inFanW) {
          if (!fanState && calibratedTemp >= fanThreshold + TEMP_HYSTERESIS) {
            fanState = true;
            applyFan();
            triggerBeep();
            Serial.printf("[FAN] AUTO ON: Temp %.1f°C >= %.1f°C\n", 
                         calibratedTemp, fanThreshold + TEMP_HYSTERESIS);
          } else if (fanState && calibratedTemp <= fanThreshold - TEMP_HYSTERESIS) {
            fanState = false;
            applyFan();
            Serial.printf("[FAN] AUTO OFF: Temp %.1f°C <= %.1f°C\n", 
                         calibratedTemp, fanThreshold - TEMP_HYSTERESIS);
          }
        } else {
          if (fanState) {
            fanState = false;
            applyFan();
            Serial.println("[FAN] AUTO OFF: Outside time window");
          }
        }
      }

      xSemaphoreGive(stateMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ─────────────────────────────────────────
//  BUTTON TASK (Core 1)
// ─────────────────────────────────────────
int readButton() {
  int val = analogRead(BUTTON_ADC_PIN);
  if (val >= 200  && val <= 400)  return 1;
  if (val >= 500  && val <= 600)  return 2;
  if (val >= 1100 && val <= 1200) return 3;
  if (val >= 1900 && val <= 2000) return 4;
  if (val >= 2700 && val <= 2800) return 5;
  return 0;
}

void buttonTask(void* pvParameters) {
  esp_task_wdt_add(NULL);
  unsigned long lastPress = 0;

  for (;;) {
    esp_task_wdt_reset();

    unsigned long now = millis();
    int btn = readButton();

    if (btn != 0 && now - lastPress >= 2000) {
      lastPress = now;
      Serial.printf("[BUTTON] Pressed: %d\n", btn);

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool needSave = false;
        
        switch (btn) {
          case 1:
            masterEnabled = !masterEnabled;
            Serial.printf("[MASTER] %s via Button 1\n", masterEnabled ? "ON" : "OFF");
            if (!masterEnabled) {
              fanState = false;
              lightsState = false;
              lightsAutoOn = false;
              r2State = false;
              r3State = false;
              allRelaysOff();
            }
            needSave = true;
            triggerBeep();
            break;
          case 2: 
            r2State = !r2State; 
            applyR2(); 
            triggerBeep(); 
            break;
          case 3: 
            r3State = !r3State; 
            applyR3(); 
            triggerBeep(); 
            break;
          case 4:
            switch (fanMode) {
              case FAN_AUTO: fanMode=FAN_MANUAL_ON; fanState=true; break;
              case FAN_MANUAL_ON: fanMode=FAN_MANUAL_OFF; fanState=false; break;
              case FAN_MANUAL_OFF: fanMode=FAN_AUTO; fanState=false; break;
            }
            applyFan(); 
            needSave = true;
            triggerBeep(); 
            break;
          case 5:
            if (lightsMode==MANUAL) { 
              lightsMode=AUTO; 
              lightsState=false; 
              lightsAutoOn=false; 
            } else { 
              lightsMode=MANUAL; 
              lightsState=true; 
              lightsAutoOn=false; 
            }
            applyLights(); 
            needSave = true;
            triggerBeep(); 
            break;
        }
        
        xSemaphoreGive(stateMutex);
        
        if (needSave) {
          saveSettings();
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ─────────────────────────────────────────
//  BUZZER TASK (Core 1)
// ─────────────────────────────────────────
void buzzerTask(void* pvParameters) {
  esp_task_wdt_add(NULL);
  for (;;) {
    esp_task_wdt_reset();
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (buzzerActive && millis() - buzzerStartTime >= 500) {
        ledcWrite(BUZZER_CHANNEL, 0);
        buzzerActive = false;
      }
      xSemaphoreGive(stateMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ─────────────────────────────────────────
//  BLUETOOTH TASK (Core 0)
// ─────────────────────────────────────────
void btTask(void* pvParameters) {
  esp_task_wdt_add(NULL);
  Serial.println("[BT] Task running - waiting for connections...");

  String        btBuffer      = "";
  unsigned long lastStream    = 0;
  bool          clientWas     = false;

  for (;;) {
    esp_task_wdt_reset();
    bool connected = SerialBT.hasClient();

    if (connected && !clientWas) {
      Serial.println("[BT] Client connected");
      char welcomeBuf[150];
      snprintf(welcomeBuf, sizeof(welcomeBuf), 
               "SmartTable v3.1 - BT Only (Motion Fixed)\nTime: %02d:%02d:%02d %s\nTiming: Motion=%ds Grace=%ds Buffer=%ds",
               currentTime.hour, currentTime.minute, currentTime.second,
               timeSynced ? "(synced)" : "(not set - use TIME HH:MM:SS)",
               MOTION_DEBOUNCE_TIME/1000,
               MOTION_GRACE_PERIOD/1000,
               TIME_WINDOW_EXIT_BUFFER/1000);
      SerialBT.println(welcomeBuf);
      SerialBT.println("Commands: STATUS, ON, OFF, HELP, TIME HH:MM:SS, GETTIME");
      clientWas = true;
    } else if (!connected && clientWas) {
      Serial.println("[BT] Client disconnected");
      clientWas = false;
    }

    while (SerialBT.available()) {
      char c = SerialBT.read();
      if (c == '\n' || c == '\r') {
        btBuffer.trim();
        String cmdUpper = btBuffer;
        cmdUpper.toUpperCase();

        if (btBuffer.length() > 0) {
          Serial.printf("[BT CMD] %s\n", btBuffer.c_str());
          String response = "";
          bool needSave = false;

          if (cmdUpper == "HELP") {
            response = "=== Commands ===\nSTATUS, ON/OFF, FAN ON/OFF/AUTO, LIGHT ON/OFF/AUTO\nR2 ON/OFF, R3 ON/OFF, TEMP\nSET TEMP <value>, SET LIGHTTIME <HH:MM-HH:MM>\nSET FANTIME <HH:MM-HH:MM>\nTIME HH:MM:SS, GETTIME\nSAVE, LOAD, CLEAR, RESET, REBOOT\n\nTiming: Motion=20s Grace=3s ExitBuffer=30s";
          }
          else if (cmdUpper == "STATUS") { response = buildStatus(); }
          else if (cmdUpper == "TEMP") {
            char buf[60];
            snprintf(buf, sizeof(buf), "Temperature: %.1fC | Humidity: %.0f%% | Threshold: %.1fC",
                     calibratedTemp, humidity, fanThreshold);
            response = buf;
          }
          else if (cmdUpper == "GETTIME") {
            char buf[60];
            snprintf(buf, sizeof(buf), "Time: %02d:%02d:%02d %s",
                     currentTime.hour, currentTime.minute, currentTime.second,
                     timeSynced ? "(synced)" : "(not synced)");
            response = buf;
          }
          else if (cmdUpper.startsWith("TIME ")) {
            String timeStr = btBuffer.substring(5);
            int sep1 = timeStr.indexOf(':');
            int sep2 = timeStr.lastIndexOf(':');
            if (sep1 > 0 && sep2 > sep1) {
              int hour = timeStr.substring(0, sep1).toInt();
              int minute = timeStr.substring(sep1+1, sep2).toInt();
              int second = timeStr.substring(sep2+1).toInt();
              if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59) {
                if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                  initSoftClock(hour, minute, second);
                  timeSynced = true;
                  saveTimeToRTC();
                  saveTimeToNVS();
                  triggerBeep();
                  xSemaphoreGive(stateMutex);
                  needSave = true;
                }
                char buf[60];
                snprintf(buf, sizeof(buf), "[TIME] Set to %02d:%02d:%02d (Saved to RTC+NVS)", hour, minute, second);
                response = buf;
              } else { response = "[ERROR] Invalid time. Use HH:MM:SS"; }
            } else { response = "[ERROR] Format: TIME HH:MM:SS"; }
          }
          else if (cmdUpper == "ON") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              masterEnabled = true; triggerBeep();
              xSemaphoreGive(stateMutex); needSave = true;
            }
            response = "[MASTER] ON (Saved)";
          }
          else if (cmdUpper == "OFF") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              masterEnabled = false; fanState = false; lightsState = false;
              lightsAutoOn = false; r2State = false; r3State = false;
              allRelaysOff(); triggerBeep();
              xSemaphoreGive(stateMutex); needSave = true;
            }
            response = "[MASTER] OFF (Saved)";
          }
          else if (cmdUpper == "FAN ON") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              fanMode = FAN_MANUAL_ON; fanState = true; applyFan(); triggerBeep();
              xSemaphoreGive(stateMutex); needSave = true;
            }
            response = "[FAN] Manual ON (Saved)";
          }
          else if (cmdUpper == "FAN OFF") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              fanMode = FAN_MANUAL_OFF; fanState = false; applyFan(); triggerBeep();
              xSemaphoreGive(stateMutex); needSave = true;
            }
            response = "[FAN] Manual OFF (Saved)";
          }
          else if (cmdUpper == "FAN AUTO") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              fanMode = FAN_AUTO; fanState = false; applyFan(); triggerBeep();
              xSemaphoreGive(stateMutex); needSave = true;
            }
            response = "[FAN] Auto mode (Saved)";
          }
          else if (cmdUpper == "LIGHT ON") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lightsMode = MANUAL; lightsState = true; lightsAutoOn = false; 
              applyLights(); triggerBeep();
              xSemaphoreGive(stateMutex); needSave = true;
            }
            response = "[LIGHT] Manual ON (Saved)";
          }
          else if (cmdUpper == "LIGHT OFF") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lightsMode = MANUAL; lightsState = false; lightsAutoOn = false; 
              applyLights(); triggerBeep();
              xSemaphoreGive(stateMutex); needSave = true;
            }
            response = "[LIGHT] Manual OFF (Saved)";
          }
          else if (cmdUpper == "LIGHT AUTO") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lightsMode = AUTO; lightsState = false; lightsAutoOn = false; 
              applyLights(); triggerBeep();
              xSemaphoreGive(stateMutex); needSave = true;
            }
            response = "[LIGHT] Auto mode (Saved)";
          }
          else if (cmdUpper == "R2 ON") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              r2State = true; applyR2(); triggerBeep();
              xSemaphoreGive(stateMutex);
            }
            response = "[R2] ON";
          }
          else if (cmdUpper == "R2 OFF") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              r2State = false; applyR2(); triggerBeep();
              xSemaphoreGive(stateMutex);
            }
            response = "[R2] OFF";
          }
          else if (cmdUpper == "R3 ON") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              r3State = true; applyR3(); triggerBeep();
              xSemaphoreGive(stateMutex);
            }
            response = "[R3] ON";
          }
          else if (cmdUpper == "R3 OFF") {
            if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              r3State = false; applyR3(); triggerBeep();
              xSemaphoreGive(stateMutex);
            }
            response = "[R3] OFF";
          }
          else if (cmdUpper == "SAVE") { saveSettings(); response = "[NVS] Saved"; }
          else if (cmdUpper == "LOAD") { loadSettings(); applyAll(); response = "[NVS] Loaded"; }
          else if (cmdUpper == "CLEAR") { clearSettings(); response = "[NVS] Cleared"; }
          else if (cmdUpper == "REBOOT") {
            saveSettings(); SerialBT.println("[SYS] Rebooting..."); delay(500); ESP.restart();
          }
          else { response = "Unknown: " + btBuffer + " - Type HELP"; }

          if (needSave) saveSettings();
          if (response.length() > 0) { SerialBT.println(response); Serial.println(response); }
        }
        btBuffer = "";
      } else { btBuffer += c; }
    }

    if (connected && millis() - lastStream >= 5000) {
      lastStream = millis();
      char buf[200];
      snprintf(buf, sizeof(buf),
               "[LIVE] %02d:%02d:%02d | T:%.1fC H:%.0f%% | PIR:%s Dark:%s | Fan:%s(%s) Light:%s(%s) | Master:%s | Motion:%s",
               currentTime.hour, currentTime.minute, currentTime.second,
               calibratedTemp, humidity,
               motionDetected?"YES":"NO", isDark?"YES":"NO",
               fanState?"ON":"OFF", (fanMode==FAN_AUTO)?"AUTO":((fanMode==FAN_MANUAL_ON)?"MAN":"OFF"),
               lightsState?"ON":"OFF", lightsMode==MANUAL?"MAN":"AUTO",
               masterEnabled?"ON":"OFF",
               motionStable?"STABLE":"-");
      SerialBT.println(buf);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ─────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);

  for (int i = 0; i < 4; i++) {
    digitalWrite(relayPins[i], RELAY_OFF);
    pinMode(relayPins[i], OUTPUT);
  }
  pinMode(BUTTON_ADC_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);

  ledcSetup(BUZZER_CHANNEL, BUZZER_FREQ, BUZZER_RES);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  dht.begin();

  if (SerialBT.begin(BT_DEVICE_NAME)) {
    Serial.println("[BT] Bluetooth initialized. Device: " BT_DEVICE_NAME);
  } else {
    Serial.println("[BT] Bluetooth init failed!");
  }
  delay(500);

  Serial.println("\n========================================");
  Serial.println("=== Smart Table v3.1 - Motion Fixed ===");
  Serial.printf("=== Motion: %ds CONTINUOUS + %ds grace | Buffer: %ds ===\n", 
                MOTION_DEBOUNCE_TIME/1000, 
                MOTION_GRACE_PERIOD/1000,
                TIME_WINDOW_EXIT_BUFFER/1000);
  Serial.println("========================================\n");

  stateMutex = xSemaphoreCreateMutex();
  if (stateMutex == NULL) {
    Serial.println("[ERROR] Failed to create mutex!");
    return;
  }

  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();

  loadSettings();
  
  // Try to restore time from RTC memory or NVS
  Serial.println("[BOOT] Attempting to restore time from memory...");
  bool timeRestored = restoreTimeFromMemory();
  
  if (timeRestored) {
    Serial.printf("[BOOT] Time restored: %02d:%02d:%02d\n",
                  currentTime.hour, currentTime.minute, currentTime.second);
  } else {
    Serial.println("[BOOT] No saved time - set via BT: TIME HH:MM:SS");
  }

  xTaskCreatePinnedToCore(sensorTask,  "SensorTask",  4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(logicTask,   "LogicTask",   4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(buttonTask,  "ButtonTask",  3072, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(buzzerTask,  "BuzzerTask",  2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(btTask,      "BTTask",      8192, NULL, 1, NULL, 0);

  Serial.println("[RTOS] All tasks running");
  Serial.println("[BT]   Ready: " BT_DEVICE_NAME);
  Serial.println("\n=== System Ready ===\n");
  
  esp_task_wdt_delete(NULL);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}