#ifndef CONFIG_H
#define CONFIG_H

// ============================================
// WiFi Configuration
// ============================================
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ============================================
// Location Configuration
// ============================================
// Set your center location (latitude/longitude)
// Default: London Heathrow area
#define CENTER_LAT 51.4700
#define CENTER_LON -0.4543

// Radius in nautical miles to track aircraft around your location
// This defines the bounding box for the API query
#define RADIUS_NM 25.0

// ============================================
// OpenSky Network Configuration
// ============================================
// API endpoint (free, no auth required for anonymous access)
// Anonymous: 10 second rate limit, 400 API credits/day
// Registered: 5 second rate limit, 4000 API credits/day
// Register free at: https://opensky-network.org/
#define OPENSKY_BASE_URL "https://opensky-network.org/api/states/all"

// Optional: Set credentials for higher rate limits (leave empty for anonymous)
#define OPENSKY_USERNAME ""
#define OPENSKY_PASSWORD ""

// ============================================
// Display Configuration
// ============================================
// GC9A01 240x240 round display
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 240
#define SCREEN_CENTER_X 120
#define SCREEN_CENTER_Y 120
#define SCREEN_RADIUS 118  // Slightly less than 120 to avoid edge clipping

// ============================================
// Update Intervals
// ============================================
// How often to poll the API (milliseconds)
// OpenSky anonymous: minimum 10 seconds between requests
#define API_POLL_INTERVAL_MS 15000

// How often to redraw the display (milliseconds) 
// Interpolates aircraft positions between API calls
#define DISPLAY_REFRESH_MS 1000

// ============================================
// Aircraft Display
// ============================================
// Maximum number of aircraft to track simultaneously
#define MAX_AIRCRAFT 20

// Minimum altitude to display (feet) - filters ground vehicles
#define MIN_ALTITUDE_FT 500

// ============================================
// Pin Definitions for GC9A01 display
// ============================================
// Using default VSPI pins on ESP32
// These are configured in TFT_eSPI User_Setup.h
// but listed here for reference:
//   TFT_MOSI  = GPIO 23
//   TFT_SCLK  = GPIO 18
//   TFT_CS    = GPIO 5
//   TFT_DC    = GPIO 2
//   TFT_RST   = GPIO 4
//   TFT_BL    = GPIO 15 (backlight, optional)

#endif // CONFIG_H
