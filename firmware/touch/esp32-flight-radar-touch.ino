/**
 * ESP32 Flight Radar - Touch Display Edition
 * 
 * Displays live aircraft on a 1.28" GC9A01 round TFT display with touch.
 * Tap on any aircraft to see flight details (callsign, altitude, speed, route).
 * 
 * Hardware:
 *   - ESP32 (any variant with WiFi)
 *   - 1.28" GC9A01 240x240 round TFT display with CST816S touch (SPI + I2C)
 * 
 * Libraries required:
 *   - TFT_eSPI (display driver)
 *   - ArduinoJson (JSON parsing)
 *   - CST816S or similar touch library
 *   - WiFi (built-in)
 *   - HTTPClient (built-in)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>

#include "config.h"

// ============================================
// Touch Configuration (CST816S via I2C)
// ============================================
#define TOUCH_SDA  21
#define TOUCH_SCL  22
#define TOUCH_INT  16
#define TOUCH_RST  17
#define CST816S_ADDR 0x15

// ============================================
// Data Structures
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
  bool onGround;
  bool active;
  unsigned long lastSeen;
  int16_t screenX;
  int16_t screenY;
  float prevLat;
  float prevLon;
};

// Route info (fetched on tap)
struct RouteInfo {
  char origin[5];
  char destination[5];
  bool loaded;
  bool loading;
};

// App states
enum AppState {
  STATE_RADAR,      // Normal radar view
  STATE_DETAIL      // Aircraft detail popup
};

// ============================================
// Global State
// ============================================
TFT_eSPI tft = TFT_eSPI();

Aircraft aircraft[MAX_AIRCRAFT];
int aircraftCount = 0;
RouteInfo currentRoute;
int selectedAircraftIdx = -1;
AppState appState = STATE_RADAR;

unsigned long lastApiCall = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTouchCheck = 0;
bool wifiConnected = false;

float bboxLatMin, bboxLatMax, bboxLonMin, bboxLonMax;

// ============================================
// Colors
// ============================================
#define COLOR_BG        TFT_BLACK
#define COLOR_GRID      0x1082
#define COLOR_RING      0x2945
#define COLOR_CENTER    TFT_GREEN
#define COLOR_AIRCRAFT  TFT_YELLOW
#define COLOR_TEXT      TFT_WHITE
#define COLOR_CALLSIGN  TFT_CYAN
#define COLOR_BORDER    0x4A49
#define COLOR_POPUP_BG  0x1082
#define COLOR_POPUP_HDR 0x2124
#define COLOR_ROUTE     0xFFE0  // Yellow

// ============================================
// Touch Functions
// ============================================
bool touchAvailable = false;
int16_t touchX = -1, touchY = -1;
bool touchPressed = false;

void initTouch() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  
  // Reset touch controller
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(10);
  digitalWrite(TOUCH_RST, HIGH);
  delay(50);
  
  // Check if touch controller responds
  Wire.beginTransmission(CST816S_ADDR);
  if (Wire.endTransmission() == 0) {
    touchAvailable = true;
    Serial.println("Touch controller found (CST816S)");
  } else {
    Serial.println("No touch controller found - tap disabled");
  }
}

bool readTouch() {
  if (!touchAvailable) return false;
  
  Wire.beginTransmission(CST816S_ADDR);
  Wire.write(0x01);  // Gesture/touch register
  Wire.endTransmission();
  
  Wire.requestFrom(CST816S_ADDR, 6);
  if (Wire.available() < 6) return false;
  
  uint8_t gesture = Wire.read();
  uint8_t points = Wire.read();
  uint8_t xHigh = Wire.read();
  uint8_t xLow = Wire.read();
  uint8_t yHigh = Wire.read();
  uint8_t yLow = Wire.read();
  
  if (points > 0) {
    touchX = ((xHigh & 0x0F) << 8) | xLow;
    touchY = ((yHigh & 0x0F) << 8) | yLow;
    touchPressed = true;
    return true;
  }
  
  touchPressed = false;
  return false;
}

// ============================================
// Setup
// ============================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 Flight Radar (Touch) ===");
  
  calculateBoundingBox();
  initDisplay();
  initTouch();
  drawSplashScreen();
  delay(2000);
  connectWiFi();
  memset(aircraft, 0, sizeof(aircraft));
  memset(&currentRoute, 0, sizeof(currentRoute));
  drawRadarBackground();
}

// ============================================
// Main Loop
// ============================================
void loop() {
  unsigned long now = millis();
  
  // WiFi reconnect
  if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    wifiConnected = false;
    connectWiFi();
  }
  
  // Handle touch input (check every 50ms)
  if (now - lastTouchCheck >= 50) {
    lastTouchCheck = now;
    handleTouch();
  }
  
  // Poll API (only in radar view)
  if (appState == STATE_RADAR && now - lastApiCall >= API_POLL_INTERVAL_MS) {
    lastApiCall = now;
    fetchAircraftData();
  }
  
  // Update display
  if (now - lastDisplayUpdate >= DISPLAY_REFRESH_MS) {
    lastDisplayUpdate = now;
    if (appState == STATE_RADAR) {
      interpolatePositions();
      drawRadarScreen();
    }
  }
  
  yield();
}

// ============================================
// Touch Handler
// ============================================
void handleTouch() {
  static bool wasTouched = false;
  bool isTouched = readTouch();
  
  // Only trigger on touch-down (not hold)
  if (isTouched && !wasTouched) {
    if (appState == STATE_DETAIL) {
      // Any tap dismisses the detail popup
      appState = STATE_RADAR;
      selectedAircraftIdx = -1;
      drawRadarScreen();
    } else {
      // Try to find aircraft near touch point
      int tapped = findAircraftAtPoint(touchX, touchY);
      if (tapped >= 0) {
        selectedAircraftIdx = tapped;
        appState = STATE_DETAIL;
        showDetailPopup(tapped);
      }
    }
  }
  wasTouched = isTouched;
}

int findAircraftAtPoint(int16_t tx, int16_t ty) {
  int closest = -1;
  float closestDist = 20.0;  // Max 20px to register a tap
  
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    if (!aircraft[i].active) continue;
    
    float dx = aircraft[i].screenX - tx;
    float dy = aircraft[i].screenY - ty;
    float dist = sqrt(dx * dx + dy * dy);
    
    if (dist < closestDist) {
      closestDist = dist;
      closest = i;
    }
  }
  return closest;
}

// ============================================
// Detail Popup Display
// ============================================
void showDetailPopup(int idx) {
  Aircraft& a = aircraft[idx];
  
  float altFt = a.altitude * 3.28084;
  float speedKts = a.velocity * 1.944;
  int fl = (int)(altFt / 100);
  
  // Clear screen and draw popup
  tft.fillScreen(COLOR_BG);
  
  // Header bar
  tft.fillRect(0, 0, 240, 40, COLOR_POPUP_HDR);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COLOR_ROUTE, COLOR_POPUP_HDR);
  tft.drawString(a.callsign, 10, 15, 4);
  
  tft.setTextColor(COLOR_TEXT, COLOR_POPUP_HDR);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(a.icao24, 230, 15, 2);
  
  tft.setTextColor(0x8410, COLOR_POPUP_HDR);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(a.country, 230, 32, 1);
  
  // Route section (origin -> destination)
  tft.setTextDatum(MC_DATUM);
  
  // Fetch route if not already cached
  fetchRouteInfo(a.callsign);
  
  if (currentRoute.loaded) {
    // Origin
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(currentRoute.origin, 60, 70, 4);
    tft.setTextColor(0x8410, COLOR_BG);
    tft.drawString("DEP", 60, 92, 1);
    
    // Arrow
    tft.setTextColor(COLOR_ROUTE, COLOR_BG);
    tft.drawString(">>>>>", 120, 70, 2);
    
    // Destination
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.drawString(currentRoute.destination, 180, 70, 4);
    tft.setTextColor(0x8410, COLOR_BG);
    tft.drawString("ARR", 180, 92, 1);
  } else {
    tft.setTextColor(0x8410, COLOR_BG);
    tft.drawString("Route unavailable", 120, 75, 2);
  }
  
  // Divider line
  tft.drawLine(20, 108, 220, 108, COLOR_GRID);
  
  // Flight details - 2x2 grid
  // Altitude
  tft.setTextColor(0x8410, COLOR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("ALTITUDE", 60, 122, 1);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  char altStr[16];
  snprintf(altStr, sizeof(altStr), "%d ft", (int)altFt);
  tft.drawString(altStr, 60, 138, 2);
  tft.setTextColor(getAltitudeColor(a.altitude), COLOR_BG);
  char flStr[8];
  snprintf(flStr, sizeof(flStr), "FL%d", fl);
  tft.drawString(flStr, 60, 155, 1);
  
  // Speed
  tft.setTextColor(0x8410, COLOR_BG);
  tft.drawString("SPEED", 180, 122, 1);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  char spdStr[16];
  snprintf(spdStr, sizeof(spdStr), "%d kts", (int)speedKts);
  tft.drawString(spdStr, 180, 138, 2);
  tft.setTextColor(0x8410, COLOR_BG);
  char kmhStr[16];
  snprintf(kmhStr, sizeof(kmhStr), "%d km/h", (int)(a.velocity * 3.6));
  tft.drawString(kmhStr, 180, 155, 1);
  
  // Heading
  tft.setTextColor(0x8410, COLOR_BG);
  tft.drawString("HEADING", 60, 172, 1);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  char hdgStr[16];
  snprintf(hdgStr, sizeof(hdgStr), "%d%c", (int)a.heading, 0xB0);
  tft.drawString(hdgStr, 60, 188, 2);
  tft.setTextColor(0x8410, COLOR_BG);
  tft.drawString(getCompassDir(a.heading), 60, 205, 1);
  
  // Vertical rate
  tft.setTextColor(0x8410, COLOR_BG);
  tft.drawString("V/S", 180, 172, 1);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  char vsStr[16];
  int vsFpm = (int)(a.verticalRate * 196.85);  // m/s to ft/min
  snprintf(vsStr, sizeof(vsStr), "%+d", vsFpm);
  tft.drawString(vsStr, 180, 188, 2);
  tft.setTextColor(0x8410, COLOR_BG);
  tft.drawString("ft/min", 180, 205, 1);
  
  // Bottom: tap to dismiss
  tft.setTextColor(0x4A49, COLOR_BG);
  tft.drawString("Tap anywhere to close", 120, 228, 1);
}

const char* getCompassDir(float deg) {
  static const char* dirs[] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                                "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  int idx = ((int)(deg / 22.5 + 0.5)) % 16;
  return dirs[idx];
}

// ============================================
// Route Lookup (OpenSky Routes API)
// ============================================
void fetchRouteInfo(const char* callsign) {
  memset(&currentRoute, 0, sizeof(currentRoute));
  currentRoute.loading = true;
  
  if (WiFi.status() != WL_CONNECTED) {
    currentRoute.loading = false;
    return;
  }
  
  String url = "https://opensky-network.org/api/routes?callsign=" + String(callsign);
  Serial.println("Fetching route: " + url);
  
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    
    if (!err) {
      JsonArray route = doc["route"];
      if (route.size() >= 2) {
        strncpy(currentRoute.origin, route[0].as<const char*>(), 4);
        currentRoute.origin[4] = '\0';
        strncpy(currentRoute.destination, route[route.size()-1].as<const char*>(), 4);
        currentRoute.destination[4] = '\0';
        currentRoute.loaded = true;
      }
    }
  } else {
    Serial.printf("Route lookup failed: %d\n", httpCode);
  }
  
  currentRoute.loading = false;
  http.end();
}

// ============================================
// WiFi Connection
// ============================================
void connectWiFi() {
  tft.fillScreen(COLOR_BG);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting WiFi...", SCREEN_CENTER_X, SCREEN_CENTER_Y - 10, 2);
  tft.drawString(WIFI_SSID, SCREEN_CENTER_X, SCREEN_CENTER_Y + 10, 2);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    tft.fillScreen(COLOR_BG);
    tft.setTextColor(COLOR_CENTER, COLOR_BG);
    tft.drawString("Connected!", SCREEN_CENTER_X, SCREEN_CENTER_Y, 2);
    delay(1000);
  } else {
    tft.fillScreen(COLOR_BG);
    tft.setTextColor(TFT_RED, COLOR_BG);
    tft.drawString("WiFi Failed!", SCREEN_CENTER_X, SCREEN_CENTER_Y, 2);
    delay(3000);
  }
}

// ============================================
// Bounding Box Calculation
// ============================================
void calculateBoundingBox() {
  float radiusKm = RADIUS_NM * 1.852;
  float latDelta = radiusKm / 111.12;
  float lonDelta = radiusKm / (111.12 * cos(CENTER_LAT * PI / 180.0));
  bboxLatMin = CENTER_LAT - latDelta;
  bboxLatMax = CENTER_LAT + latDelta;
  bboxLonMin = CENTER_LON - lonDelta;
  bboxLonMax = CENTER_LON + lonDelta;
}

// ============================================
// OpenSky Network API
// ============================================
void fetchAircraftData() {
  if (WiFi.status() != WL_CONNECTED) return;
  
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
  DeserializationError error = deserializeJson(doc, json);
  if (error) return;
  
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
    
    float lat = state[6].as<float>();
    float lon = state[5].as<float>();
    float alt = state[7].as<float>();
    bool onGround = state[8].as<bool>();
    
    if (state[5].isNull() || state[6].isNull()) continue;
    if (onGround) continue;
    if (alt * 3.28084 < MIN_ALTITUDE_FT) continue;
    
    int slot = findSlot(state[0].as<const char*>());
    if (slot < 0) continue;
    
    if (aircraft[slot].active) {
      aircraft[slot].prevLat = aircraft[slot].latitude;
      aircraft[slot].prevLon = aircraft[slot].longitude;
    } else {
      aircraft[slot].prevLat = lat;
      aircraft[slot].prevLon = lon;
    }
    
    const char* icao = state[0].as<const char*>();
    const char* call = state[1].as<const char*>();
    const char* country = state[2].as<const char*>();
    
    strncpy(aircraft[slot].icao24, icao ? icao : "???", 6);
    aircraft[slot].icao24[6] = '\0';
    
    if (call) {
      strncpy(aircraft[slot].callsign, call, 8);
      aircraft[slot].callsign[8] = '\0';
      for (int j = strlen(aircraft[slot].callsign) - 1; j >= 0 && aircraft[slot].callsign[j] == ' '; j--)
        aircraft[slot].callsign[j] = '\0';
    } else {
      strncpy(aircraft[slot].callsign, aircraft[slot].icao24, 8);
    }
    
    if (country) strncpy(aircraft[slot].country, country, 19);
    aircraft[slot].country[19] = '\0';
    
    aircraft[slot].latitude = lat;
    aircraft[slot].longitude = lon;
    aircraft[slot].altitude = alt;
    aircraft[slot].velocity = state[9].isNull() ? 0 : state[9].as<float>();
    aircraft[slot].heading = state[10].isNull() ? 0 : state[10].as<float>();
    aircraft[slot].verticalRate = state[11].isNull() ? 0 : state[11].as<float>();
    aircraft[slot].onGround = onGround;
    aircraft[slot].active = true;
    aircraft[slot].lastSeen = millis();
    count++;
  }
  aircraftCount = count;
}

int findSlot(const char* icao24) {
  if (!icao24) return -1;
  for (int i = 0; i < MAX_AIRCRAFT; i++)
    if (aircraft[i].active && strcmp(aircraft[i].icao24, icao24) == 0) return i;
  for (int i = 0; i < MAX_AIRCRAFT; i++)
    if (!aircraft[i].active) return i;
  return -1;
}

// ============================================
// Position Interpolation
// ============================================
void interpolatePositions() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    if (!aircraft[i].active) continue;
    if (now - aircraft[i].lastSeen > 60000) { aircraft[i].active = false; continue; }
    
    float elapsed = (now - aircraft[i].lastSeen) / 1000.0;
    if (elapsed > 0 && aircraft[i].velocity > 0) {
      float distKm = (aircraft[i].velocity * elapsed) / 1000.0;
      float headingRad = aircraft[i].heading * PI / 180.0;
      aircraft[i].latitude += (distKm * cos(headingRad)) / 111.12;
      aircraft[i].longitude += (distKm * sin(headingRad)) / (111.12 * cos(aircraft[i].latitude * PI / 180.0));
    }
    geoToScreen(aircraft[i].latitude, aircraft[i].longitude, &aircraft[i].screenX, &aircraft[i].screenY);
  }
}

void geoToScreen(float lat, float lon, int16_t* sx, int16_t* sy) {
  float dLatKm = (lat - CENTER_LAT) * 111.12;
  float dLonKm = (lon - CENTER_LON) * 111.12 * cos(CENTER_LAT * PI / 180.0);
  float dLatNm = dLatKm / 1.852;
  float dLonNm = dLonKm / 1.852;
  float scale = (float)SCREEN_RADIUS / RADIUS_NM;
  *sx = SCREEN_CENTER_X + (int16_t)(dLonNm * scale);
  *sy = SCREEN_CENTER_Y - (int16_t)(dLatNm * scale);
}

// ============================================
// Display Drawing
// ============================================
void initDisplay() {
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(COLOR_BG);
  tft.setTextDatum(MC_DATUM);
}

void drawSplashScreen() {
  tft.fillScreen(COLOR_BG);
  for (int r = 20; r <= 100; r += 20)
    tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, r, COLOR_GRID);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.drawString("FLIGHT", SCREEN_CENTER_X, SCREEN_CENTER_Y - 20, 4);
  tft.drawString("RADAR", SCREEN_CENTER_X, SCREEN_CENTER_Y + 15, 4);
  tft.setTextColor(COLOR_CALLSIGN, COLOR_BG);
  tft.drawString("Touch Edition", SCREEN_CENTER_X, SCREEN_CENTER_Y + 45, 2);
}

void drawRadarBackground() {
  tft.fillScreen(COLOR_BG);
  drawRadarGrid();
}

void drawRadarGrid() {
  tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, SCREEN_RADIUS, COLOR_BORDER);
  tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, SCREEN_RADIUS - 1, COLOR_BORDER);
  for (int i = 1; i <= 4; i++) {
    int r = (SCREEN_RADIUS * i) / 4;
    tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, r, COLOR_RING);
  }
  tft.drawLine(SCREEN_CENTER_X, SCREEN_CENTER_Y - SCREEN_RADIUS,
               SCREEN_CENTER_X, SCREEN_CENTER_Y + SCREEN_RADIUS, COLOR_GRID);
  tft.drawLine(SCREEN_CENTER_X - SCREEN_RADIUS, SCREEN_CENTER_Y,
               SCREEN_CENTER_X + SCREEN_RADIUS, SCREEN_CENTER_Y, COLOR_GRID);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COLOR_RING, COLOR_BG);
  tft.drawString("N", SCREEN_CENTER_X, 8, 1);
  tft.drawString("S", SCREEN_CENTER_X, 232, 1);
  tft.drawString("E", 232, SCREEN_CENTER_Y, 1);
  tft.drawString("W", 8, SCREEN_CENTER_Y, 1);
  tft.fillCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, 3, COLOR_CENTER);
}

void drawRadarScreen() {
  tft.fillScreen(COLOR_BG);
  drawRadarGrid();
  drawAircraftIcons();
  drawInfoBar();
}

void drawAircraftIcons() {
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    if (!aircraft[i].active) continue;
    int16_t x = aircraft[i].screenX;
    int16_t y = aircraft[i].screenY;
    float dx = x - SCREEN_CENTER_X;
    float dy = y - SCREEN_CENTER_Y;
    if (sqrt(dx*dx + dy*dy) > SCREEN_RADIUS - 5) continue;
    
    uint16_t color = getAltitudeColor(aircraft[i].altitude);
    drawPlaneIcon(x, y, aircraft[i].heading, color);
    
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(COLOR_CALLSIGN, COLOR_BG);
    int16_t lx = x + 8;
    if (lx > 200) lx = x - 40;
    tft.drawString(aircraft[i].callsign, lx, y - 4, 1);
    
    char altStr[8];
    snprintf(altStr, sizeof(altStr), "FL%d", (int)(aircraft[i].altitude * 3.28084 / 100));
    tft.setTextColor(color, COLOR_BG);
    tft.drawString(altStr, lx, y + 5, 1);
  }
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

uint16_t getAltitudeColor(float altMeters) {
  float altFt = altMeters * 3.28084;
  if (altFt < 5000) return TFT_ORANGE;
  if (altFt < 15000) return TFT_YELLOW;
  if (altFt < 30000) return TFT_GREEN;
  if (altFt < 40000) return TFT_CYAN;
  return TFT_MAGENTA;
}

void drawInfoBar() {
  char info[32];
  snprintf(info, sizeof(info), "%d aircraft", aircraftCount);
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.drawString(info, SCREEN_CENTER_X, 235, 1);
  char rangeStr[16];
  snprintf(rangeStr, sizeof(rangeStr), "%.0fNM", RADIUS_NM);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(rangeStr, SCREEN_CENTER_X, 15, 1);
}
