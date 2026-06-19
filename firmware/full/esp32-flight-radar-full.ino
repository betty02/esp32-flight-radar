/**
 * ESP32 Flight Radar - Full Standalone Edition v2.1
 * 
 * Three-page interface controlled by rotary encoder:
 *   LEFT  = Clock/Date display
 *   CENTER = Live radar view (default)
 *   RIGHT = Settings page
 * 
 * Features:
 *   - WiFi captive portal (hold button on boot)
 *   - Battery level monitoring with on-screen indicator
 *   - Backlight brightness control (long-press encoder)
 *   - NTP time sync with analogue clock face
 *   - Settings persist to flash (NVS)
 *   - Touch support for aircraft detail popup
 * 
 * Hardware:
 *   - ESP32 with WiFi
 *   - 1.28" GC9A01 240x240 round TFT with touch + rotary encoder
 *   - Optional: LiPo battery with voltage divider on ADC
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>

#include "config.h"

// ============================================
// Pages / App State
// ============================================
enum Page {
  PAGE_CLOCK = 0,
  PAGE_RADAR = 1,
  PAGE_SETTINGS = 2
};

enum AppMode {
  MODE_NORMAL,
  MODE_BRIGHTNESS,     // Adjusting brightness with encoder
  MODE_PORTAL          // WiFi captive portal active
};

Page currentPage = PAGE_RADAR;
AppMode appMode = MODE_NORMAL;

// ============================================
// Settings (stored in flash)
// ============================================
Preferences prefs;

struct Settings {
  char wifiSSID[33];
  char wifiPassword[65];
  float centerLat;
  float centerLon;
  float radiusNm;
  int8_t gmtOffset;
  bool use24hr;
  uint8_t brightness;
} settings;

// ============================================
// Aircraft Data
// ============================================
struct Aircraft {
  char icao24[7];
  char callsign[9];
  char country[20];
  float latitude;
  float longitude;
  float altitude;
  float velocity;
  float heading;
  float verticalRate;
  bool active;
  unsigned long lastSeen;
  int16_t screenX;
  int16_t screenY;
};

Aircraft aircraft[MAX_AIRCRAFT];
int aircraftCount = 0;

// ============================================
// Rotary Encoder State
// ============================================
volatile int encoderPos = 0;
volatile bool encoderBtnPressed = false;
int lastEncoderPos = 0;
unsigned long lastBtnPress = 0;
unsigned long btnDownTime = 0;
bool btnHeld = false;
bool btnCurrentlyDown = false;

// ============================================
// Battery Monitoring
// ============================================
float batteryVoltage = 0.0;
int batteryPercent = -1;  // -1 = no battery detected
unsigned long lastBatteryRead = 0;

// ============================================
// Brightness
// ============================================
unsigned long brightnessDisplayUntil = 0;

// ============================================
// Captive Portal
// ============================================
WebServer* portalServer = nullptr;
DNSServer* dnsServer = nullptr;
bool portalActive = false;
bool portalConfigSaved = false;

// ============================================
// Display & Timing
// ============================================
TFT_eSPI tft = TFT_eSPI();

unsigned long lastApiCall = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastNtpSync = 0;
bool wifiConnected = false;
bool timeConfigured = false;

float bboxLatMin, bboxLatMax, bboxLonMin, bboxLonMax;

// Settings page state
int settingsMenuItem = 0;
bool settingsEditing = false;
#define SETTINGS_ITEMS 7  // SSID, Password, Lat, Lon, Radius, GMT Offset, Brightness

// ============================================
// Encoder ISR
// ============================================
void IRAM_ATTR encoderISR() {
  static uint8_t lastState = 0;
  uint8_t a = digitalRead(ENCODER_PIN_A);
  uint8_t b = digitalRead(ENCODER_PIN_B);
  uint8_t state = (a << 1) | b;
  uint8_t transition = (lastState << 2) | state;
  switch (transition) {
    case 0b0001: case 0b0111: case 0b1110: case 0b1000:
      encoderPos++; break;
    case 0b0010: case 0b1011: case 0b1101: case 0b0100:
      encoderPos--; break;
  }
  lastState = state;
}

void IRAM_ATTR btnISR() {
  // We handle press/release timing in loop() for long-press detection
}

// ============================================
// Setup
// ============================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 Flight Radar v2.1 ===");
  
  // Load settings from flash
  loadSettings();
  
  // Init display
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  
  // Init backlight PWM
  ledcSetup(0, 5000, 8);  // Channel 0, 5kHz, 8-bit resolution
  ledcAttachPin(TFT_BACKLIGHT_PIN, 0);
  setBrightness(settings.brightness);
  
  // Init encoder pins
  pinMode(ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);
  pinMode(ENCODER_PIN_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN_B), encoderISR, CHANGE);
  
  // Init battery ADC
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogSetAttenuation(ADC_11db);  // Full 0-3.3V range
  readBattery();  // Initial reading
  
  // Check if button is held on boot -> enter captive portal
  if (checkPortalBootTrigger()) {
    startCaptivePortal();
    return;  // Portal mode takes over
  }
  
  // Normal boot: splash screen
  drawSplash();
  delay(2000);
  
  // Connect WiFi
  connectWiFi();
  
  // Configure NTP time
  if (wifiConnected) {
    configTime(settings.gmtOffset * 3600, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    timeConfigured = true;
  }
  
  // Calculate bounding box
  calculateBoundingBox();
  
  // Init aircraft array
  memset(aircraft, 0, sizeof(aircraft));
  
  // Draw initial page
  drawCurrentPage();
}

// ============================================
// Main Loop
// ============================================
void loop() {
  unsigned long now = millis();
  
  // If captive portal is active, only run portal logic
  if (appMode == MODE_PORTAL) {
    loopCaptivePortal();
    return;
  }
  
  // Handle encoder rotation
  handleEncoder();
  
  // Handle button (with long-press detection)
  handleButton();
  
  // Brightness overlay timeout
  if (appMode == MODE_BRIGHTNESS && now > brightnessDisplayUntil) {
    appMode = MODE_NORMAL;
    drawCurrentPage();
  }
  
  // WiFi check
  if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    wifiConnected = false;
  }
  
  // Battery monitoring
  if (now - lastBatteryRead >= BATTERY_READ_INTERVAL_MS) {
    lastBatteryRead = now;
    readBattery();
  }
  
  // Page-specific updates
  switch (currentPage) {
    case PAGE_CLOCK:
      if (now - lastDisplayUpdate >= CLOCK_REFRESH_MS) {
        lastDisplayUpdate = now;
        drawClockPage();
      }
      break;
      
    case PAGE_RADAR:
      if (wifiConnected && now - lastApiCall >= API_POLL_INTERVAL_MS) {
        lastApiCall = now;
        fetchAircraftData();
      }
      if (now - lastDisplayUpdate >= DISPLAY_REFRESH_MS) {
        lastDisplayUpdate = now;
        interpolatePositions();
        drawRadarPage();
      }
      break;
      
    case PAGE_SETTINGS:
      // Static, redraws on encoder input only
      break;
  }
  
  yield();
}

// ============================================
// WiFi Captive Portal
// ============================================
bool checkPortalBootTrigger() {
  // Check if encoder button is held during boot
  Serial.println("Checking for portal trigger (hold button)...");
  unsigned long start = millis();
  
  while (digitalRead(ENCODER_PIN_BTN) == LOW) {
    if (millis() - start >= PORTAL_BOOT_HOLD_MS) {
      Serial.println("Portal trigger detected!");
      return true;
    }
    delay(10);
  }
  return false;
}

void startCaptivePortal() {
  appMode = MODE_PORTAL;
  portalActive = true;
  
  // Show portal screen
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, SCREEN_RADIUS, 0x4A49);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("WiFi Setup", SCREEN_CENTER_X, 40, 4);
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connect to WiFi:", SCREEN_CENTER_X, 80, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(AP_SSID, SCREEN_CENTER_X, 105, 2);
  
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Then open:", SCREEN_CENTER_X, 140, 2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("192.168.4.1", SCREEN_CENTER_X, 165, 2);
  
  tft.setTextColor(0x8410, TFT_BLACK);
  tft.drawString("in your browser", SCREEN_CENTER_X, 190, 1);
  tft.drawString("Press button to exit", SCREEN_CENTER_X, 220, 1);
  
  // Start AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, strlen(AP_PASSWORD) > 0 ? AP_PASSWORD : nullptr);
  Serial.printf("AP started: %s at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  
  // Start DNS (redirect all domains to us)
  dnsServer = new DNSServer();
  dnsServer->start(53, "*", WiFi.softAPIP());
  
  // Start web server
  portalServer = new WebServer(80);
  portalServer->on("/", HTTP_GET, handlePortalRoot);
  portalServer->on("/save", HTTP_POST, handlePortalSave);
  portalServer->on("/scan", HTTP_GET, handlePortalScan);
  portalServer->onNotFound(handlePortalRoot);  // Captive portal redirect
  portalServer->begin();
}

void loopCaptivePortal() {
  if (dnsServer) dnsServer->processNextRequest();
  if (portalServer) portalServer->handleClient();
  
  // Check button to exit portal
  if (digitalRead(ENCODER_PIN_BTN) == LOW) {
    delay(200);
    if (digitalRead(ENCODER_PIN_BTN) == LOW) {
      stopCaptivePortal();
    }
  }
  
  // If config was saved, restart
  if (portalConfigSaved) {
    delay(2000);
    ESP.restart();
  }
}

void stopCaptivePortal() {
  if (portalServer) { portalServer->stop(); delete portalServer; portalServer = nullptr; }
  if (dnsServer) { dnsServer->stop(); delete dnsServer; dnsServer = nullptr; }
  WiFi.softAPdisconnect(true);
  portalActive = false;
  appMode = MODE_NORMAL;
  
  // Restart to apply settings
  ESP.restart();
}

void handlePortalRoot() {
  String html = "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Flight Radar Setup</title>"
    "<style>"
    "body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;padding:20px;max-width:400px;margin:0 auto;}"
    "h1{color:#00ff88;text-align:center;}"
    "label{display:block;margin:12px 0 4px;color:#888;font-size:0.9em;}"
    "input,select{width:100%;padding:10px;border:1px solid #333;background:#0f1629;"
    "color:#e0e0e0;border-radius:4px;font-size:1em;box-sizing:border-box;}"
    "input:focus{border-color:#00ff88;outline:none;}"
    "button{width:100%;padding:12px;background:#00ff88;color:#1a1a2e;border:none;"
    "border-radius:4px;font-size:1.1em;font-weight:bold;cursor:pointer;margin-top:20px;}"
    "button:hover{background:#00cc6a;}"
    ".networks{background:#0f1629;border:1px solid #333;border-radius:4px;padding:8px;margin:8px 0;"
    "max-height:150px;overflow-y:auto;}"
    ".net-item{padding:6px;cursor:pointer;border-bottom:1px solid #1a1a2e;}"
    ".net-item:hover{background:#1a2a4e;}"
    ".info{text-align:center;color:#666;font-size:0.8em;margin-top:15px;}"
    "</style></head><body>"
    "<h1>&#9992; Flight Radar</h1>"
    "<form action='/save' method='POST'>"
    "<label>WiFi Network</label>"
    "<input type='text' name='ssid' value='" + String(settings.wifiSSID) + "' placeholder='WiFi name'>"
    "<div id='networks' class='networks'><em>Scanning...</em></div>"
    "<label>WiFi Password</label>"
    "<input type='password' name='pass' placeholder='WiFi password'>"
    "<label>Latitude</label>"
    "<input type='number' name='lat' step='0.0001' value='" + String(settings.centerLat, 4) + "'>"
    "<label>Longitude</label>"
    "<input type='number' name='lon' step='0.0001' value='" + String(settings.centerLon, 4) + "'>"
    "<label>Radius (Nautical Miles)</label>"
    "<input type='number' name='radius' min='5' max='100' step='5' value='" + String((int)settings.radiusNm) + "'>"
    "<label>UTC Offset (hours)</label>"
    "<input type='number' name='gmt' min='-12' max='14' value='" + String(settings.gmtOffset) + "'>"
    "<button type='submit'>Save &amp; Restart</button>"
    "</form>"
    "<p class='info'>After saving, the device will restart and connect to your WiFi.</p>"
    "<script>"
    "fetch('/scan').then(r=>r.json()).then(nets=>{"
    "let el=document.getElementById('networks');"
    "if(!nets.length){el.innerHTML='<em>No networks found</em>';return;}"
    "el.innerHTML=nets.map(n=>'<div class=\"net-item\" onclick=\"document.querySelector([name=ssid]).value=\\''+n+'\\'\">'+n+'</div>').join('');"
    "});"
    "</script></body></html>";
  
  portalServer->send(200, "text/html", html);
}

void handlePortalSave() {
  if (portalServer->hasArg("ssid")) {
    String ssid = portalServer->arg("ssid");
    String pass = portalServer->arg("pass");
    strncpy(settings.wifiSSID, ssid.c_str(), 32);
    settings.wifiSSID[32] = '\0';
    strncpy(settings.wifiPassword, pass.c_str(), 64);
    settings.wifiPassword[64] = '\0';
  }
  if (portalServer->hasArg("lat"))
    settings.centerLat = portalServer->arg("lat").toFloat();
  if (portalServer->hasArg("lon"))
    settings.centerLon = portalServer->arg("lon").toFloat();
  if (portalServer->hasArg("radius"))
    settings.radiusNm = portalServer->arg("radius").toFloat();
  if (portalServer->hasArg("gmt"))
    settings.gmtOffset = portalServer->arg("gmt").toInt();
  
  saveSettings();
  
  String html = "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#1a1a2e;color:#e0e0e0;padding:40px;text-align:center;}"
    "h1{color:#00ff88;}</style></head><body>"
    "<h1>&#10004; Saved!</h1><p>Device is restarting...</p>"
    "<p>It will connect to: <strong>" + String(settings.wifiSSID) + "</strong></p>"
    "</body></html>";
  
  portalServer->send(200, "text/html", html);
  portalConfigSaved = true;
  
  // Show saved on display
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("Settings Saved!", SCREEN_CENTER_X, SCREEN_CENTER_Y - 10, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Restarting...", SCREEN_CENTER_X, SCREEN_CENTER_Y + 20, 2);
}

void handlePortalScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n && i < 15; i++) {
    if (i > 0) json += ",";
    json += "\"" + WiFi.SSID(i) + "\"";
  }
  json += "]";
  portalServer->send(200, "application/json", json);
}

// ============================================
// Battery Monitoring
// ============================================
void readBattery() {
  // Read ADC multiple times and average for stability
  long total = 0;
  for (int i = 0; i < 16; i++) {
    total += analogRead(BATTERY_ADC_PIN);
    delayMicroseconds(100);
  }
  float adcValue = total / 16.0;
  
  // Convert ADC reading to voltage
  // ESP32 ADC: 12-bit (0-4095), 0-3.3V with 11dB attenuation
  float adcVoltage = (adcValue / 4095.0) * 3.3;
  batteryVoltage = adcVoltage * BATTERY_DIVIDER_RATIO;
  
  // Calculate percentage
  if (batteryVoltage < 1.0) {
    // No battery connected (reading near zero)
    batteryPercent = -1;
  } else {
    float pct = (batteryVoltage - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100.0;
    batteryPercent = constrain((int)pct, 0, 100);
  }
  
  Serial.printf("Battery: %.2fV (%d%%)\n", batteryVoltage, batteryPercent);
}

void drawBatteryIndicator() {
  // Draw battery icon in top-left corner of radar/clock pages
  if (batteryPercent < 0) return;  // No battery, don't draw
  
  int bx = 8;   // Top-left position
  int by = 6;
  int bw = 22;  // Battery body width
  int bh = 11;  // Battery body height
  
  // Battery outline
  tft.drawRect(bx, by, bw, bh, 0x8410);
  // Battery tip (positive terminal)
  tft.fillRect(bx + bw, by + 3, 2, 5, 0x8410);
  
  // Fill level
  int fillWidth = ((bw - 4) * batteryPercent) / 100;
  uint16_t fillColor;
  if (batteryPercent > 60) fillColor = TFT_GREEN;
  else if (batteryPercent > 20) fillColor = TFT_YELLOW;
  else fillColor = TFT_RED;
  
  if (fillWidth > 0) {
    tft.fillRect(bx + 2, by + 2, fillWidth, bh - 4, fillColor);
  }
  
  // Clear unfilled area
  int emptyStart = bx + 2 + fillWidth;
  int emptyWidth = (bw - 4) - fillWidth;
  if (emptyWidth > 0) {
    tft.fillRect(emptyStart, by + 2, emptyWidth, bh - 4, TFT_BLACK);
  }
  
  // Percentage text next to battery (small)
  char pctStr[5];
  snprintf(pctStr, sizeof(pctStr), "%d%%", batteryPercent);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(0x8410, TFT_BLACK);
  tft.drawString(pctStr, bx + bw + 5, by + bh / 2, 1);
}

// ============================================
// Brightness Control
// ============================================
void setBrightness(uint8_t level) {
  level = constrain(level, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
  ledcWrite(0, level);
  settings.brightness = level;
}

void drawBrightnessOverlay() {
  // Draw a centered brightness indicator overlay
  int barWidth = 160;
  int barHeight = 20;
  int barX = SCREEN_CENTER_X - barWidth / 2;
  int barY = SCREEN_CENTER_Y + 30;
  
  // Semi-transparent background area
  tft.fillRoundRect(SCREEN_CENTER_X - 90, SCREEN_CENTER_Y - 30, 180, 80, 10, 0x1082);
  tft.drawRoundRect(SCREEN_CENTER_X - 90, SCREEN_CENTER_Y - 30, 180, 80, 10, 0x4A49);
  
  // Brightness icon (sun)
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x1082);
  tft.drawString("BRIGHTNESS", SCREEN_CENTER_X, SCREEN_CENTER_Y - 12, 2);
  
  // Bar background
  tft.fillRoundRect(barX, barY, barWidth, barHeight, 4, 0x2945);
  
  // Bar fill
  int fillWidth = ((barWidth - 4) * settings.brightness) / 255;
  uint16_t barColor = TFT_YELLOW;
  if (settings.brightness < 80) barColor = TFT_ORANGE;
  tft.fillRoundRect(barX + 2, barY + 2, fillWidth, barHeight - 4, 3, barColor);
  
  // Percentage
  char pctStr[5];
  snprintf(pctStr, sizeof(pctStr), "%d%%", (settings.brightness * 100) / 255);
  tft.setTextColor(TFT_WHITE, 0x1082);
  tft.drawString(pctStr, SCREEN_CENTER_X, barY + barHeight + 12, 1);
}

// ============================================
// Button Handling (with long-press detection)
// ============================================
void handleButton() {
  bool btnState = (digitalRead(ENCODER_PIN_BTN) == LOW);  // Active low
  unsigned long now = millis();
  
  if (btnState && !btnCurrentlyDown) {
    // Button just pressed down
    btnCurrentlyDown = true;
    btnDownTime = now;
    btnHeld = false;
  }
  
  if (btnState && btnCurrentlyDown && !btnHeld) {
    // Button still held - check for long press
    if (now - btnDownTime >= LONG_PRESS_MS) {
      btnHeld = true;
      // Long press detected -> toggle brightness mode
      if (appMode == MODE_BRIGHTNESS) {
        appMode = MODE_NORMAL;
        saveSettings();
        drawCurrentPage();
      } else {
        appMode = MODE_BRIGHTNESS;
        brightnessDisplayUntil = now + 3000;  // Show for 3s after last interaction
        drawCurrentPage();
        drawBrightnessOverlay();
      }
    }
  }
  
  if (!btnState && btnCurrentlyDown) {
    // Button released
    btnCurrentlyDown = false;
    
    if (!btnHeld && (now - btnDownTime < LONG_PRESS_MS)) {
      // Short press (wasn't a long-press)
      if (now - lastBtnPress > 200) {  // Debounce
        lastBtnPress = now;
        handleShortPress();
      }
    }
  }
}

void handleShortPress() {
  if (appMode == MODE_BRIGHTNESS) {
    // Exit brightness mode on short press too
    appMode = MODE_NORMAL;
    saveSettings();
    drawCurrentPage();
  } else if (currentPage == PAGE_SETTINGS) {
    if (settingsEditing) {
      settingsEditing = false;
      saveSettings();
      calculateBoundingBox();
      drawSettingsPage();
    } else {
      settingsEditing = true;
      drawSettingsPage();
    }
  } else if (currentPage == PAGE_RADAR) {
    // Force refresh
    fetchAircraftData();
  }
}

// ============================================
// Encoder Handling
// ============================================
void handleEncoder() {
  int pos = encoderPos;
  int delta = pos - lastEncoderPos;
  if (delta == 0) return;
  lastEncoderPos = pos;
  
  if (appMode == MODE_BRIGHTNESS) {
    // Adjust brightness
    int newBright = (int)settings.brightness + (delta * BRIGHTNESS_STEP);
    newBright = constrain(newBright, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    setBrightness((uint8_t)newBright);
    brightnessDisplayUntil = millis() + 3000;
    drawCurrentPage();
    drawBrightnessOverlay();
    return;
  }
  
  if (currentPage == PAGE_SETTINGS && settingsEditing) {
    adjustSettingValue(settingsMenuItem, delta);
    drawSettingsPage();
  } else if (currentPage == PAGE_SETTINGS) {
    settingsMenuItem += (delta > 0) ? 1 : -1;
    if (settingsMenuItem < 0) {
      settingsMenuItem = 0;
      currentPage = PAGE_RADAR;
      drawCurrentPage();
    } else if (settingsMenuItem >= SETTINGS_ITEMS) {
      settingsMenuItem = SETTINGS_ITEMS - 1;
    } else {
      drawSettingsPage();
    }
  } else {
    // Page switching
    int newPage = (int)currentPage + (delta > 0 ? 1 : -1);
    if (newPage < PAGE_CLOCK) newPage = PAGE_CLOCK;
    if (newPage > PAGE_SETTINGS) newPage = PAGE_SETTINGS;
    
    if (newPage != (int)currentPage) {
      currentPage = (Page)newPage;
      drawCurrentPage();
    }
  }
}

void adjustSettingValue(int item, int delta) {
  switch (item) {
    case 0: // SSID - show portal hint
      break;
    case 1: // Password - show portal hint
      break;
    case 2: // Latitude
      settings.centerLat += delta * 0.01;
      settings.centerLat = constrain(settings.centerLat, -90.0, 90.0);
      break;
    case 3: // Longitude
      settings.centerLon += delta * 0.01;
      settings.centerLon = constrain(settings.centerLon, -180.0, 180.0);
      break;
    case 4: // Radius
      settings.radiusNm += delta * 5.0;
      settings.radiusNm = constrain(settings.radiusNm, 5.0, 100.0);
      break;
    case 5: // GMT offset
      settings.gmtOffset += delta;
      settings.gmtOffset = constrain(settings.gmtOffset, -12, 14);
      break;
    case 6: // Brightness
      {
        int newBright = (int)settings.brightness + (delta * BRIGHTNESS_STEP);
        newBright = constrain(newBright, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
        setBrightness((uint8_t)newBright);
      }
      break;
  }
}

// ============================================
// Settings Storage (NVS Flash)
// ============================================
void loadSettings() {
  prefs.begin(PREFS_NAMESPACE, true);
  
  String ssid = prefs.getString("ssid", DEFAULT_WIFI_SSID);
  String pass = prefs.getString("pass", DEFAULT_WIFI_PASSWORD);
  strncpy(settings.wifiSSID, ssid.c_str(), 32);
  settings.wifiSSID[32] = '\0';
  strncpy(settings.wifiPassword, pass.c_str(), 64);
  settings.wifiPassword[64] = '\0';
  
  settings.centerLat = prefs.getFloat("lat", DEFAULT_CENTER_LAT);
  settings.centerLon = prefs.getFloat("lon", DEFAULT_CENTER_LON);
  settings.radiusNm = prefs.getFloat("radius", DEFAULT_RADIUS_NM);
  settings.gmtOffset = prefs.getChar("gmt", 0);
  settings.use24hr = prefs.getBool("24hr", true);
  settings.brightness = prefs.getUChar("bright", BRIGHTNESS_DEFAULT);
  
  prefs.end();
  
  Serial.printf("Loaded: lat=%.4f lon=%.4f radius=%.0f gmt=%d bright=%d\n",
                settings.centerLat, settings.centerLon, settings.radiusNm,
                settings.gmtOffset, settings.brightness);
}

void saveSettings() {
  prefs.begin(PREFS_NAMESPACE, false);
  
  prefs.putString("ssid", settings.wifiSSID);
  prefs.putString("pass", settings.wifiPassword);
  prefs.putFloat("lat", settings.centerLat);
  prefs.putFloat("lon", settings.centerLon);
  prefs.putFloat("radius", settings.radiusNm);
  prefs.putChar("gmt", settings.gmtOffset);
  prefs.putBool("24hr", settings.use24hr);
  prefs.putUChar("bright", settings.brightness);
  
  prefs.end();
  Serial.println("Settings saved");
}

// ============================================
// WiFi Connection
// ============================================
void connectWiFi() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting...", SCREEN_CENTER_X, SCREEN_CENTER_Y - 10, 2);
  tft.drawString(settings.wifiSSID, SCREEN_CENTER_X, SCREEN_CENTER_Y + 10, 2);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(settings.wifiSSID, settings.wifiPassword);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    // Animated progress dots
    int dotX = 80 + (attempts % 10) * 8;
    tft.fillCircle(dotX, SCREEN_CENTER_Y + 30, 2, TFT_CYAN);
  }
  
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  
  if (wifiConnected) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("Connected!", SCREEN_CENTER_X, SCREEN_CENTER_Y, 2);
    Serial.printf("WiFi OK: %s\n", WiFi.localIP().toString().c_str());
    delay(1000);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("WiFi Failed", SCREEN_CENTER_X, SCREEN_CENTER_Y - 10, 2);
    tft.setTextColor(0x8410, TFT_BLACK);
    tft.drawString("Hold button on boot", SCREEN_CENTER_X, SCREEN_CENTER_Y + 10, 1);
    tft.drawString("to configure WiFi", SCREEN_CENTER_X, SCREEN_CENTER_Y + 22, 1);
    Serial.println("WiFi failed - offline mode");
    delay(3000);
  }
}

// ============================================
// Bounding Box
// ============================================
void calculateBoundingBox() {
  float radiusKm = settings.radiusNm * 1.852;
  float latDelta = radiusKm / 111.12;
  float lonDelta = radiusKm / (111.12 * cos(settings.centerLat * PI / 180.0));
  bboxLatMin = settings.centerLat - latDelta;
  bboxLatMax = settings.centerLat + latDelta;
  bboxLonMin = settings.centerLon - lonDelta;
  bboxLonMax = settings.centerLon + lonDelta;
}

// ============================================
// PAGE: Clock
// ============================================
void drawClockPage() {
  tft.fillScreen(TFT_BLACK);
  
  tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, SCREEN_RADIUS, 0x2945);
  tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, SCREEN_RADIUS - 1, 0x2945);
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("No time sync", SCREEN_CENTER_X, SCREEN_CENTER_Y, 2);
    tft.setTextColor(0x8410, TFT_BLACK);
    tft.drawString("Connect WiFi for NTP", SCREEN_CENTER_X, SCREEN_CENTER_Y + 20, 1);
    drawBatteryIndicator();
    drawPageIndicator(PAGE_CLOCK);
    return;
  }
  
  // Hour markers
  for (int i = 0; i < 12; i++) {
    float angle = i * 30.0 * PI / 180.0 - PI / 2;
    int x1 = SCREEN_CENTER_X + cos(angle) * (SCREEN_RADIUS - 8);
    int y1 = SCREEN_CENTER_Y + sin(angle) * (SCREEN_RADIUS - 8);
    int x2 = SCREEN_CENTER_X + cos(angle) * (SCREEN_RADIUS - 15);
    int y2 = SCREEN_CENTER_Y + sin(angle) * (SCREEN_RADIUS - 15);
    tft.drawLine(x1, y1, x2, y2, 0x4A49);
  }
  
  // Minute markers
  for (int i = 0; i < 60; i++) {
    if (i % 5 == 0) continue;
    float angle = i * 6.0 * PI / 180.0 - PI / 2;
    int x = SCREEN_CENTER_X + cos(angle) * (SCREEN_RADIUS - 8);
    int y = SCREEN_CENTER_Y + sin(angle) * (SCREEN_RADIUS - 8);
    tft.drawPixel(x, y, 0x2945);
  }
  
  int hour = timeinfo.tm_hour;
  int minute = timeinfo.tm_min;
  int second = timeinfo.tm_sec;
  
  // Hour hand
  float hourAngle = ((hour % 12) + minute / 60.0) * 30.0 * PI / 180.0 - PI / 2;
  int hx = SCREEN_CENTER_X + cos(hourAngle) * 50;
  int hy = SCREEN_CENTER_Y + sin(hourAngle) * 50;
  tft.drawLine(SCREEN_CENTER_X, SCREEN_CENTER_Y, hx, hy, TFT_WHITE);
  tft.drawLine(SCREEN_CENTER_X + 1, SCREEN_CENTER_Y, hx + 1, hy, TFT_WHITE);
  
  // Minute hand
  float minAngle = minute * 6.0 * PI / 180.0 - PI / 2;
  int mx = SCREEN_CENTER_X + cos(minAngle) * 75;
  int my = SCREEN_CENTER_Y + sin(minAngle) * 75;
  tft.drawLine(SCREEN_CENTER_X, SCREEN_CENTER_Y, mx, my, TFT_CYAN);
  
  // Second hand
  float secAngle = second * 6.0 * PI / 180.0 - PI / 2;
  int sx = SCREEN_CENTER_X + cos(secAngle) * 85;
  int sy = SCREEN_CENTER_Y + sin(secAngle) * 85;
  tft.drawLine(SCREEN_CENTER_X, SCREEN_CENTER_Y, sx, sy, TFT_RED);
  
  // Center dot
  tft.fillCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, 4, TFT_WHITE);
  
  // Digital time
  char timeStr[9];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", hour, minute, second);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(timeStr, SCREEN_CENTER_X, SCREEN_CENTER_Y + 40, 4);
  
  // Date
  char dateStr[20];
  strftime(dateStr, sizeof(dateStr), "%a %d %b %Y", &timeinfo);
  tft.setTextColor(0x8410, TFT_BLACK);
  tft.drawString(dateStr, SCREEN_CENTER_X, SCREEN_CENTER_Y + 65, 1);
  
  // Battery and page indicator
  drawBatteryIndicator();
  drawPageIndicator(PAGE_CLOCK);
}

// ============================================
// PAGE: Radar
// ============================================
void drawRadarPage() {
  tft.fillScreen(TFT_BLACK);
  
  // Range rings
  tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, SCREEN_RADIUS, 0x4A49);
  for (int i = 1; i <= 4; i++) {
    int r = (SCREEN_RADIUS * i) / 4;
    tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, r, 0x2945);
  }
  
  // Crosshairs
  tft.drawLine(SCREEN_CENTER_X, SCREEN_CENTER_Y - SCREEN_RADIUS,
               SCREEN_CENTER_X, SCREEN_CENTER_Y + SCREEN_RADIUS, 0x1082);
  tft.drawLine(SCREEN_CENTER_X - SCREEN_RADIUS, SCREEN_CENTER_Y,
               SCREEN_CENTER_X + SCREEN_RADIUS, SCREEN_CENTER_Y, 0x1082);
  
  // Compass labels
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(0x2945, TFT_BLACK);
  tft.drawString("N", SCREEN_CENTER_X, 8, 1);
  tft.drawString("S", SCREEN_CENTER_X, 232, 1);
  tft.drawString("E", 232, SCREEN_CENTER_Y, 1);
  tft.drawString("W", 8, SCREEN_CENTER_Y, 1);
  
  // Center marker
  tft.fillCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, 3, TFT_GREEN);
  
  // Draw aircraft
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    if (!aircraft[i].active) continue;
    int16_t x = aircraft[i].screenX;
    int16_t y = aircraft[i].screenY;
    float dx = x - SCREEN_CENTER_X;
    float dy = y - SCREEN_CENTER_Y;
    if (sqrt(dx * dx + dy * dy) > SCREEN_RADIUS - 5) continue;
    
    uint16_t color = getAltColor(aircraft[i].altitude);
    drawPlaneIcon(x, y, aircraft[i].heading, color);
    
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    int16_t lx = x + 8;
    if (lx > 200) lx = x - 40;
    tft.drawString(aircraft[i].callsign, lx, y - 4, 1);
    
    char fl[8];
    snprintf(fl, sizeof(fl), "FL%d", (int)(aircraft[i].altitude * 3.28084 / 100));
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(fl, lx, y + 5, 1);
  }
  
  // Info bar
  char info[32];
  snprintf(info, sizeof(info), "%d aircraft", aircraftCount);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(info, SCREEN_CENTER_X, 235, 1);
  
  char rangeStr[16];
  snprintf(rangeStr, sizeof(rangeStr), "%.0fNM", settings.radiusNm);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(rangeStr, SCREEN_CENTER_X, 15, 1);
  
  // WiFi indicator (top-right)
  if (!wifiConnected) {
    tft.fillCircle(228, 12, 4, TFT_RED);
  } else {
    tft.fillCircle(228, 12, 4, TFT_GREEN);
  }
  
  // Battery indicator
  drawBatteryIndicator();
  
  // Page indicator
  drawPageIndicator(PAGE_RADAR);
}

// ============================================
// PAGE: Settings
// ============================================
void drawSettingsPage() {
  tft.fillScreen(TFT_BLACK);
  tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, SCREEN_RADIUS, 0x4A49);
  
  // Title
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("SETTINGS", SCREEN_CENTER_X, 12, 2);
  
  // Menu items
  const char* labels[] = {"WiFi SSID", "WiFi Pass", "Latitude", "Longitude", "Radius (NM)", "UTC Offset", "Brightness"};
  char values[7][20];
  
  // Truncate SSID if too long for display
  if (strlen(settings.wifiSSID) > 12) {
    snprintf(values[0], 20, "%.10s..", settings.wifiSSID);
  } else {
    snprintf(values[0], 20, "%s", settings.wifiSSID);
  }
  snprintf(values[1], 20, "Use portal");
  snprintf(values[2], 20, "%.4f", settings.centerLat);
  snprintf(values[3], 20, "%.4f", settings.centerLon);
  snprintf(values[4], 20, "%.0f", settings.radiusNm);
  snprintf(values[5], 20, "%+d hrs", settings.gmtOffset);
  snprintf(values[6], 20, "%d%%", (settings.brightness * 100) / 255);
  
  int startY = 38;
  int itemHeight = 26;
  
  for (int i = 0; i < SETTINGS_ITEMS; i++) {
    int y = startY + i * itemHeight;
    bool isSelected = (i == settingsMenuItem);
    bool isEditing = isSelected && settingsEditing;
    
    if (isSelected) {
      int barWidth = 180;
      int barX = SCREEN_CENTER_X - barWidth / 2;
      tft.fillRoundRect(barX, y - 1, barWidth, itemHeight - 3, 4,
                        isEditing ? 0x0018 : 0x1082);
    }
    
    // Label
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(isSelected ? TFT_WHITE : 0x8410, TFT_BLACK);
    tft.drawString(labels[i], 35, y + 6, 1);
    
    // Value
    tft.setTextDatum(MR_DATUM);
    uint16_t valColor = isEditing ? TFT_YELLOW : (isSelected ? TFT_CYAN : 0x8410);
    // WiFi items can't be edited via encoder
    if ((i == 0 || i == 1) && isSelected) {
      valColor = TFT_ORANGE;
    }
    tft.setTextColor(valColor, TFT_BLACK);
    tft.drawString(values[i], 205, y + 6, 1);
    
    if (isEditing && i >= 2) {
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.setTextDatum(ML_DATUM);
      tft.drawString("<>", 208, y + 6, 1);
    }
  }
  
  // Instructions
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(0x4A49, TFT_BLACK);
  if (settingsEditing) {
    tft.drawString("Scroll adjust | Press save", SCREEN_CENTER_X, 224, 1);
  } else if (settingsMenuItem <= 1) {
    tft.drawString("Hold btn on boot for WiFi setup", SCREEN_CENTER_X, 224, 1);
  } else {
    tft.drawString("Scroll nav | Press edit | Hold=dim", SCREEN_CENTER_X, 224, 1);
  }
  
  drawPageIndicator(PAGE_SETTINGS);
}

// ============================================
// Shared Drawing Helpers
// ============================================
void drawPageIndicator(Page activePage) {
  int dotY = 232;
  int dotSpacing = 12;
  int startX = SCREEN_CENTER_X - dotSpacing;
  
  for (int i = 0; i < 3; i++) {
    int x = startX + i * dotSpacing;
    if (i == (int)activePage) {
      tft.fillCircle(x, dotY, 3, TFT_WHITE);
    } else {
      tft.drawCircle(x, dotY, 2, 0x4A49);
    }
  }
}

void drawCurrentPage() {
  switch (currentPage) {
    case PAGE_CLOCK: drawClockPage(); break;
    case PAGE_RADAR: drawRadarPage(); break;
    case PAGE_SETTINGS: drawSettingsPage(); break;
  }
}

void drawSplash() {
  tft.fillScreen(TFT_BLACK);
  for (int r = 20; r <= 100; r += 20)
    tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, r, 0x1082);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("FLIGHT", SCREEN_CENTER_X, SCREEN_CENTER_Y - 20, 4);
  tft.drawString("RADAR", SCREEN_CENTER_X, SCREEN_CENTER_Y + 15, 4);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("v2.1", SCREEN_CENTER_X, SCREEN_CENTER_Y + 50, 2);
  
  // Show battery on splash
  drawBatteryIndicator();
}

void drawPlaneIcon(int16_t x, int16_t y, float heading, uint16_t color) {
  float rad = heading * PI / 180.0;
  int16_t size = 5;
  int16_t x1 = x + (int16_t)(size * 1.5 * sin(rad));
  int16_t y1 = y - (int16_t)(size * 1.5 * cos(rad));
  int16_t x2 = x + (int16_t)(size * sin(rad - 2.5));
  int16_t y2 = y - (int16_t)(size * cos(rad - 2.5));
  int16_t x3 = x + (int16_t)(size * sin(rad + 2.5));
  int16_t y3 = y - (int16_t)(size * cos(rad + 2.5));
  tft.fillTriangle(x1, y1, x2, y2, x3, y3, color);
}

uint16_t getAltColor(float altMeters) {
  float altFt = altMeters * 3.28084;
  if (altFt < 5000) return TFT_ORANGE;
  if (altFt < 15000) return TFT_YELLOW;
  if (altFt < 30000) return TFT_GREEN;
  if (altFt < 40000) return TFT_CYAN;
  return TFT_MAGENTA;
}

// ============================================
// Aircraft Data Fetching & Parsing
// ============================================
void fetchAircraftData() {
  if (!wifiConnected) return;
  
  String url = String(OPENSKY_BASE_URL) +
               "?lamin=" + String(bboxLatMin, 4) +
               "&lomin=" + String(bboxLonMin, 4) +
               "&lamax=" + String(bboxLatMax, 4) +
               "&lomax=" + String(bboxLonMax, 4);
  
  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  if (strlen(OPENSKY_USERNAME) > 0) {
    http.setAuthorization(OPENSKY_USERNAME, OPENSKY_PASSWORD);
  }
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    parseAircraftData(payload);
  }
  http.end();
}

void parseAircraftData(const String& json) {
  DynamicJsonDocument doc(32768);
  if (deserializeJson(doc, json)) return;
  
  JsonArray states = doc["states"];
  if (states.isNull()) {
    for (int i = 0; i < MAX_AIRCRAFT; i++) aircraft[i].active = false;
    aircraftCount = 0;
    return;
  }
  
  for (int i = 0; i < MAX_AIRCRAFT; i++) aircraft[i].active = false;
  
  int count = 0;
  for (JsonArray state : states) {
    if (count >= MAX_AIRCRAFT) break;
    if (state[5].isNull() || state[6].isNull()) continue;
    if (state[8].as<bool>()) continue;
    
    float alt = state[7].isNull() ? 0 : state[7].as<float>();
    if (alt * 3.28084 < MIN_ALTITUDE_FT) continue;
    
    const char* icao = state[0].as<const char*>();
    int slot = -1;
    for (int i = 0; i < MAX_AIRCRAFT; i++)
      if (aircraft[i].active && strcmp(aircraft[i].icao24, icao) == 0) { slot = i; break; }
    if (slot < 0)
      for (int i = 0; i < MAX_AIRCRAFT; i++)
        if (!aircraft[i].active) { slot = i; break; }
    if (slot < 0) break;
    
    strncpy(aircraft[slot].icao24, icao ? icao : "?", 6);
    aircraft[slot].icao24[6] = '\0';
    
    const char* call = state[1].as<const char*>();
    if (call) {
      strncpy(aircraft[slot].callsign, call, 8);
      aircraft[slot].callsign[8] = '\0';
      for (int j = strlen(aircraft[slot].callsign) - 1; j >= 0 && aircraft[slot].callsign[j] == ' '; j--)
        aircraft[slot].callsign[j] = '\0';
    } else {
      strncpy(aircraft[slot].callsign, aircraft[slot].icao24, 8);
    }
    
    aircraft[slot].latitude = state[6].as<float>();
    aircraft[slot].longitude = state[5].as<float>();
    aircraft[slot].altitude = alt;
    aircraft[slot].velocity = state[9].isNull() ? 0 : state[9].as<float>();
    aircraft[slot].heading = state[10].isNull() ? 0 : state[10].as<float>();
    aircraft[slot].verticalRate = state[11].isNull() ? 0 : state[11].as<float>();
    aircraft[slot].active = true;
    aircraft[slot].lastSeen = millis();
    count++;
  }
  aircraftCount = count;
}

// ============================================
// Position Interpolation & Conversion
// ============================================
void interpolatePositions() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    if (!aircraft[i].active) continue;
    if (now - aircraft[i].lastSeen > 60000) { aircraft[i].active = false; continue; }
    
    float elapsed = (now - aircraft[i].lastSeen) / 1000.0;
    if (elapsed > 0 && aircraft[i].velocity > 0) {
      float distKm = (aircraft[i].velocity * elapsed) / 1000.0;
      float headRad = aircraft[i].heading * PI / 180.0;
      aircraft[i].latitude += (distKm * cos(headRad)) / 111.12;
      aircraft[i].longitude += (distKm * sin(headRad)) / (111.12 * cos(aircraft[i].latitude * PI / 180.0));
    }
    
    float dLatKm = (aircraft[i].latitude - settings.centerLat) * 111.12;
    float dLonKm = (aircraft[i].longitude - settings.centerLon) * 111.12 * cos(settings.centerLat * PI / 180.0);
    float scale = (float)SCREEN_RADIUS / settings.radiusNm;
    aircraft[i].screenX = SCREEN_CENTER_X + (int16_t)((dLonKm / 1.852) * scale);
    aircraft[i].screenY = SCREEN_CENTER_Y - (int16_t)((dLatKm / 1.852) * scale);
  }
}
