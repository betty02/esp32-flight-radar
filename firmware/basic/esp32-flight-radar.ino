/**
 * ESP32 Flight Radar - Round Display Edition
 * 
 * Displays live aircraft on a 1.28" GC9A01 round TFT display.
 * Uses the OpenSky Network API to fetch real-time ADS-B data
 * for aircraft within a configurable radius of your location.
 * 
 * Hardware:
 *   - ESP32 (any variant with WiFi)
 *   - 1.28" GC9A01 240x240 round TFT display (SPI)
 * 
 * Libraries required:
 *   - TFT_eSPI (display driver)
 *   - ArduinoJson (JSON parsing)
 *   - WiFi (built-in)
 *   - HTTPClient (built-in)
 * 
 * Author: Simon
 * License: MIT
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <math.h>

#include "config.h"

// ============================================
// Data Structures
// ============================================
struct Aircraft {
  char icao24[7];       // ICAO 24-bit address
  char callsign[9];     // Callsign (flight number)
  float latitude;
  float longitude;
  float altitude;       // Barometric altitude in meters
  float velocity;       // Ground speed in m/s
  float heading;        // True track in degrees (clockwise from north)
  bool onGround;
  bool active;          // Is this slot in use?
  unsigned long lastSeen; // millis() when last updated
  // Screen position (calculated)
  int16_t screenX;
  int16_t screenY;
  // Previous position for interpolation
  float prevLat;
  float prevLon;
};

// ============================================
// Global State
// ============================================
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);  // Double-buffer sprite

Aircraft aircraft[MAX_AIRCRAFT];
int aircraftCount = 0;

unsigned long lastApiCall = 0;
unsigned long lastDisplayUpdate = 0;
bool wifiConnected = false;

// Bounding box calculated from center + radius
float bboxLatMin, bboxLatMax, bboxLonMin, bboxLonMax;

// ============================================
// Color Palette
// ============================================
#define COLOR_BG        TFT_BLACK
#define COLOR_GRID      0x1082       // Dark grey grid lines
#define COLOR_RING      0x2945       // Medium grey for range rings
#define COLOR_CENTER    TFT_GREEN    // Center point (your location)
#define COLOR_AIRCRAFT  TFT_YELLOW   // Aircraft blip
#define COLOR_TRAIL     0x4208       // Dim trail color
#define COLOR_TEXT      TFT_WHITE    // Info text
#define COLOR_CALLSIGN  TFT_CYAN    // Callsign label
#define COLOR_LOW_ALT   TFT_ORANGE  // Low altitude aircraft
#define COLOR_HIGH_ALT  TFT_GREEN   // High altitude aircraft
#define COLOR_BORDER    0x4A49       // Circle border

// ============================================
// Setup
// ============================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 Flight Radar ===");
  
  // Calculate bounding box from center location and radius
  calculateBoundingBox();
  
  // Initialize display
  initDisplay();
  
  // Show splash screen
  drawSplashScreen();
  delay(2000);
  
  // Connect to WiFi
  connectWiFi();
  
  // Initialize aircraft array
  memset(aircraft, 0, sizeof(aircraft));
  
  // Draw initial radar screen
  drawRadarBackground();
}

// ============================================
// Main Loop
// ============================================
void loop() {
  unsigned long now = millis();
  
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected) {
      wifiConnected = false;
      Serial.println("WiFi disconnected! Reconnecting...");
      connectWiFi();
    }
  }
  
  // Poll the API at the configured interval
  if (now - lastApiCall >= API_POLL_INTERVAL_MS) {
    lastApiCall = now;
    fetchAircraftData();
  }
  
  // Update display more frequently (interpolate positions)
  if (now - lastDisplayUpdate >= DISPLAY_REFRESH_MS) {
    lastDisplayUpdate = now;
    interpolatePositions();
    drawRadarScreen();
  }
  
  // Small yield to prevent watchdog reset
  yield();
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
    Serial.print(".");
    attempts++;
    
    // Animated dots on screen
    int dotX = 80 + (attempts % 10) * 8;
    tft.fillCircle(dotX, SCREEN_CENTER_Y + 30, 2, COLOR_AIRCRAFT);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    
    tft.fillScreen(COLOR_BG);
    tft.setTextColor(COLOR_CENTER, COLOR_BG);
    tft.drawString("Connected!", SCREEN_CENTER_X, SCREEN_CENTER_Y, 2);
    delay(1000);
  } else {
    Serial.println("\nWiFi failed!");
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
  // 1 nautical mile = 1.852 km
  // 1 degree latitude ≈ 111.12 km
  // 1 degree longitude ≈ 111.12 * cos(latitude) km
  
  float radiusKm = RADIUS_NM * 1.852;
  float latDelta = radiusKm / 111.12;
  float lonDelta = radiusKm / (111.12 * cos(CENTER_LAT * PI / 180.0));
  
  bboxLatMin = CENTER_LAT - latDelta;
  bboxLatMax = CENTER_LAT + latDelta;
  bboxLonMin = CENTER_LON - lonDelta;
  bboxLonMax = CENTER_LON + lonDelta;
  
  Serial.printf("Bounding box: lat[%.4f, %.4f] lon[%.4f, %.4f]\n",
                bboxLatMin, bboxLatMax, bboxLonMin, bboxLonMax);
}

// ============================================
// OpenSky Network API
// ============================================
void fetchAircraftData() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  // Build API URL with bounding box
  String url = String(OPENSKY_BASE_URL) +
               "?lamin=" + String(bboxLatMin, 4) +
               "&lomin=" + String(bboxLonMin, 4) +
               "&lamax=" + String(bboxLatMax, 4) +
               "&lomax=" + String(bboxLonMax, 4);
  
  Serial.println("Fetching: " + url);
  
  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  
  // Add auth if configured
  if (strlen(OPENSKY_USERNAME) > 0) {
    http.setAuthorization(OPENSKY_USERNAME, OPENSKY_PASSWORD);
  }
  
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String payload = http.getString();
    parseAircraftData(payload);
  } else {
    Serial.printf("HTTP error: %d\n", httpCode);
    if (httpCode == 429) {
      Serial.println("Rate limited! Waiting...");
    }
  }
  
  http.end();
}

void parseAircraftData(const String& json) {
  // OpenSky returns a large JSON, use streaming filter
  // Response format: {"time": ..., "states": [[icao24, callsign, origin_country, ...]]}
  
  // Use a large document - aircraft data can be substantial
  DynamicJsonDocument doc(32768);  // 32KB buffer
  
  DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.printf("JSON parse error: %s\n", error.c_str());
    return;
  }
  
  JsonArray states = doc["states"];
  if (states.isNull()) {
    Serial.println("No aircraft in range");
    // Mark all aircraft inactive
    for (int i = 0; i < MAX_AIRCRAFT; i++) {
      aircraft[i].active = false;
    }
    aircraftCount = 0;
    return;
  }
  
  // Mark all existing aircraft as potentially stale
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    aircraft[i].active = false;
  }
  
  int count = 0;
  for (JsonArray state : states) {
    if (count >= MAX_AIRCRAFT) break;
    
    // OpenSky state vector indices:
    // [0] icao24, [1] callsign, [2] origin_country
    // [3] time_position, [4] last_contact
    // [5] longitude, [6] latitude, [7] baro_altitude
    // [8] on_ground, [9] velocity, [10] true_track
    // [11] vertical_rate, [12] sensors
    // [13] geo_altitude, [14] squawk, [15] spi
    // [16] position_source
    
    float lat = state[6].as<float>();
    float lon = state[5].as<float>();
    float alt = state[7].as<float>();  // meters
    bool onGround = state[8].as<bool>();
    
    // Skip if no position data
    if (state[5].isNull() || state[6].isNull()) continue;
    
    // Skip ground vehicles / parked aircraft
    if (onGround) continue;
    
    // Skip very low aircraft (below min altitude)
    float altFt = alt * 3.28084;
    if (altFt < MIN_ALTITUDE_FT) continue;
    
    // Find existing slot or use a new one
    int slot = findAircraftSlot(state[0].as<const char*>());
    if (slot < 0) continue;  // No free slots
    
    // Store previous position for interpolation
    if (aircraft[slot].active) {
      aircraft[slot].prevLat = aircraft[slot].latitude;
      aircraft[slot].prevLon = aircraft[slot].longitude;
    } else {
      aircraft[slot].prevLat = lat;
      aircraft[slot].prevLon = lon;
    }
    
    // Update aircraft data
    const char* icao = state[0].as<const char*>();
    const char* call = state[1].as<const char*>();
    
    strncpy(aircraft[slot].icao24, icao ? icao : "???", 6);
    aircraft[slot].icao24[6] = '\0';
    
    // Trim whitespace from callsign
    if (call) {
      strncpy(aircraft[slot].callsign, call, 8);
      aircraft[slot].callsign[8] = '\0';
      // Trim trailing spaces
      for (int j = strlen(aircraft[slot].callsign) - 1; j >= 0 && aircraft[slot].callsign[j] == ' '; j--) {
        aircraft[slot].callsign[j] = '\0';
      }
    } else {
      strncpy(aircraft[slot].callsign, aircraft[slot].icao24, 8);
    }
    
    aircraft[slot].latitude = lat;
    aircraft[slot].longitude = lon;
    aircraft[slot].altitude = alt;
    aircraft[slot].velocity = state[9].isNull() ? 0 : state[9].as<float>();
    aircraft[slot].heading = state[10].isNull() ? 0 : state[10].as<float>();
    aircraft[slot].onGround = onGround;
    aircraft[slot].active = true;
    aircraft[slot].lastSeen = millis();
    
    count++;
  }
  
  aircraftCount = count;
  Serial.printf("Tracking %d aircraft\n", aircraftCount);
}

int findAircraftSlot(const char* icao24) {
  if (!icao24) return -1;
  
  // First: find existing entry for this aircraft
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    if (aircraft[i].active && strcmp(aircraft[i].icao24, icao24) == 0) {
      return i;
    }
  }
  
  // Second: find an empty slot
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    if (!aircraft[i].active) {
      return i;
    }
  }
  
  return -1;  // Full
}

// ============================================
// Position Interpolation
// ============================================
void interpolatePositions() {
  unsigned long now = millis();
  
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    if (!aircraft[i].active) continue;
    
    // Age out aircraft not seen for 60 seconds
    if (now - aircraft[i].lastSeen > 60000) {
      aircraft[i].active = false;
      continue;
    }
    
    // Simple dead reckoning based on velocity and heading
    float elapsed = (now - aircraft[i].lastSeen) / 1000.0;  // seconds
    if (elapsed > 0 && aircraft[i].velocity > 0) {
      float distKm = (aircraft[i].velocity * elapsed) / 1000.0;  // km
      float headingRad = aircraft[i].heading * PI / 180.0;
      
      // Approximate position update
      float latDelta = (distKm * cos(headingRad)) / 111.12;
      float lonDelta = (distKm * sin(headingRad)) / (111.12 * cos(aircraft[i].latitude * PI / 180.0));
      
      aircraft[i].latitude += latDelta;
      aircraft[i].longitude += lonDelta;
    }
    
    // Convert lat/lon to screen coordinates
    geoToScreen(aircraft[i].latitude, aircraft[i].longitude,
                &aircraft[i].screenX, &aircraft[i].screenY);
  }
}

// ============================================
// Coordinate Conversion
// ============================================
void geoToScreen(float lat, float lon, int16_t* sx, int16_t* sy) {
  // Map geographic coordinates to screen pixel coordinates
  // Center of screen = CENTER_LAT, CENTER_LON
  // Screen radius maps to RADIUS_NM nautical miles
  
  // Calculate relative position in km
  float dLatKm = (lat - CENTER_LAT) * 111.12;
  float dLonKm = (lon - CENTER_LON) * 111.12 * cos(CENTER_LAT * PI / 180.0);
  
  // Convert km to nautical miles
  float dLatNm = dLatKm / 1.852;
  float dLonNm = dLonKm / 1.852;
  
  // Map to screen pixels
  // Screen radius in pixels / radius in NM = pixels per NM
  float scale = (float)SCREEN_RADIUS / RADIUS_NM;
  
  *sx = SCREEN_CENTER_X + (int16_t)(dLonNm * scale);
  *sy = SCREEN_CENTER_Y - (int16_t)(dLatNm * scale);  // Y is inverted on screen
}

// ============================================
// Display Drawing
// ============================================
void initDisplay() {
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(COLOR_BG);
  tft.setTextDatum(MC_DATUM);
  
  // Create sprite for double buffering (full screen is too large for ESP32 RAM)
  // We'll draw directly to the TFT instead
}

void drawSplashScreen() {
  tft.fillScreen(COLOR_BG);
  
  // Draw radar-style concentric circles
  for (int r = 20; r <= 100; r += 20) {
    tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, r, COLOR_GRID);
  }
  
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.drawString("FLIGHT", SCREEN_CENTER_X, SCREEN_CENTER_Y - 20, 4);
  tft.drawString("RADAR", SCREEN_CENTER_X, SCREEN_CENTER_Y + 15, 4);
  
  tft.setTextColor(COLOR_CALLSIGN, COLOR_BG);
  tft.drawString("ESP32 Edition", SCREEN_CENTER_X, SCREEN_CENTER_Y + 45, 2);
  
  char radiusStr[32];
  snprintf(radiusStr, sizeof(radiusStr), "%.0f NM radius", RADIUS_NM);
  tft.setTextColor(COLOR_RING, COLOR_BG);
  tft.drawString(radiusStr, SCREEN_CENTER_X, SCREEN_CENTER_Y + 65, 1);
}

void drawRadarBackground() {
  tft.fillScreen(COLOR_BG);
  drawRadarGrid();
}

void drawRadarGrid() {
  // Outer circle border (mask to round display shape)
  tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, SCREEN_RADIUS, COLOR_BORDER);
  tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, SCREEN_RADIUS - 1, COLOR_BORDER);
  
  // Range rings (concentric circles showing distance)
  int numRings = 4;
  for (int i = 1; i <= numRings; i++) {
    int r = (SCREEN_RADIUS * i) / numRings;
    tft.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, r, COLOR_RING);
  }
  
  // Crosshair lines (N-S and E-W)
  tft.drawLine(SCREEN_CENTER_X, SCREEN_CENTER_Y - SCREEN_RADIUS,
               SCREEN_CENTER_X, SCREEN_CENTER_Y + SCREEN_RADIUS, COLOR_GRID);
  tft.drawLine(SCREEN_CENTER_X - SCREEN_RADIUS, SCREEN_CENTER_Y,
               SCREEN_CENTER_X + SCREEN_RADIUS, SCREEN_CENTER_Y, COLOR_GRID);
  
  // Compass labels
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COLOR_RING, COLOR_BG);
  tft.drawString("N", SCREEN_CENTER_X, 8, 1);
  tft.drawString("S", SCREEN_CENTER_X, 232, 1);
  tft.drawString("E", 232, SCREEN_CENTER_Y, 1);
  tft.drawString("W", 8, SCREEN_CENTER_Y, 1);
  
  // Center marker (your location)
  tft.fillCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, 3, COLOR_CENTER);
}

void drawRadarScreen() {
  // Redraw the full screen (clear, draw grid, draw aircraft)
  tft.fillScreen(COLOR_BG);
  drawRadarGrid();
  drawAircraft();
  drawInfoBar();
}

void drawAircraft() {
  for (int i = 0; i < MAX_AIRCRAFT; i++) {
    if (!aircraft[i].active) continue;
    
    int16_t x = aircraft[i].screenX;
    int16_t y = aircraft[i].screenY;
    
    // Check if within the circular display area
    float dx = x - SCREEN_CENTER_X;
    float dy = y - SCREEN_CENTER_Y;
    float dist = sqrt(dx * dx + dy * dy);
    if (dist > SCREEN_RADIUS - 5) continue;  // Outside visible area
    
    // Color based on altitude
    uint16_t color = getAltitudeColor(aircraft[i].altitude);
    
    // Draw aircraft icon (triangle pointing in direction of travel)
    drawPlaneIcon(x, y, aircraft[i].heading, color);
    
    // Draw callsign label
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(COLOR_CALLSIGN, COLOR_BG);
    
    // Offset label to avoid overlapping the icon
    int16_t labelX = x + 8;
    int16_t labelY = y - 4;
    
    // Keep label within screen bounds
    if (labelX > 200) labelX = x - 40;
    
    tft.drawString(aircraft[i].callsign, labelX, labelY, 1);
    
    // Draw altitude in hundreds of feet (flight level style)
    char altStr[8];
    int flightLevel = (int)(aircraft[i].altitude * 3.28084 / 100);
    snprintf(altStr, sizeof(altStr), "FL%d", flightLevel);
    tft.setTextColor(color, COLOR_BG);
    tft.drawString(altStr, labelX, labelY + 9, 1);
  }
}

void drawPlaneIcon(int16_t x, int16_t y, float heading, uint16_t color) {
  // Draw a small triangle/arrow pointing in the heading direction
  float rad = heading * PI / 180.0;
  
  // Triangle vertices (pointing up = north = 0 degrees)
  int16_t size = 5;
  
  // Nose (front)
  int16_t x1 = x + (int16_t)(size * 1.5 * sin(rad));
  int16_t y1 = y - (int16_t)(size * 1.5 * cos(rad));
  
  // Left wing
  int16_t x2 = x + (int16_t)(size * sin(rad - 2.5));
  int16_t y2 = y - (int16_t)(size * cos(rad - 2.5));
  
  // Right wing
  int16_t x3 = x + (int16_t)(size * sin(rad + 2.5));
  int16_t y3 = y - (int16_t)(size * cos(rad + 2.5));
  
  tft.fillTriangle(x1, y1, x2, y2, x3, y3, color);
  
  // Small dot at center for visibility
  tft.drawPixel(x, y, COLOR_TEXT);
}

uint16_t getAltitudeColor(float altMeters) {
  // Color gradient based on altitude
  float altFt = altMeters * 3.28084;
  
  if (altFt < 5000) return TFT_ORANGE;        // Low
  if (altFt < 15000) return TFT_YELLOW;       // Medium-low
  if (altFt < 30000) return TFT_GREEN;        // Medium
  if (altFt < 40000) return TFT_CYAN;         // High
  return TFT_MAGENTA;                          // Very high
}

void drawInfoBar() {
  // Bottom info - aircraft count and status
  char info[32];
  snprintf(info, sizeof(info), "%d aircraft", aircraftCount);
  
  tft.setTextDatum(BC_DATUM);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.drawString(info, SCREEN_CENTER_X, 235, 1);
  
  // Top info - range
  char rangeStr[16];
  snprintf(rangeStr, sizeof(rangeStr), "%.0fNM", RADIUS_NM);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(rangeStr, SCREEN_CENTER_X, 15, 1);
}
