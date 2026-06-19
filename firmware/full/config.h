#ifndef CONFIG_H
#define CONFIG_H

// ============================================
// WiFi Configuration (defaults - editable via captive portal)
// ============================================
#define DEFAULT_WIFI_SSID "YOUR_WIFI_SSID"
#define DEFAULT_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ============================================
// WiFi Captive Portal (AP mode)
// ============================================
// Hold encoder button on boot for 3 seconds to enter AP mode
#define AP_SSID "FlightRadar-Setup"
#define AP_PASSWORD ""               // Empty = open network
#define AP_TIMEOUT_SEC 180           // Auto-exit portal after 3 minutes if no connection
#define PORTAL_BOOT_HOLD_MS 3000    // Hold button this long during boot to enter portal

// ============================================
// Location (defaults - editable via settings/portal)
// ============================================
#define DEFAULT_CENTER_LAT 51.4700
#define DEFAULT_CENTER_LON -0.4543
#define DEFAULT_RADIUS_NM 25.0

// ============================================
// OpenSky Network API
// ============================================
#define OPENSKY_BASE_URL "https://opensky-network.org/api/states/all"
#define OPENSKY_USERNAME ""
#define OPENSKY_PASSWORD ""

// ============================================
// Display - GC9A01 240x240 round
// ============================================
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 240
#define SCREEN_CENTER_X 120
#define SCREEN_CENTER_Y 120
#define SCREEN_RADIUS 118

// ============================================
// Timing
// ============================================
#define API_POLL_INTERVAL_MS 15000
#define DISPLAY_REFRESH_MS 1000
#define CLOCK_REFRESH_MS 1000
#define NTP_SYNC_INTERVAL_MS 3600000  // Re-sync time every hour

// ============================================
// Aircraft
// ============================================
#define MAX_AIRCRAFT 20
#define MIN_ALTITUDE_FT 500

// ============================================
// Pin Definitions - Display (SPI)
// ============================================
// Configured via platformio.ini build flags:
//   TFT_MOSI  = GPIO 23
//   TFT_SCLK  = GPIO 18
//   TFT_CS    = GPIO 5
//   TFT_DC    = GPIO 2
//   TFT_RST   = GPIO 4
//   TFT_BL    = GPIO 15

#define TFT_BACKLIGHT_PIN 15

// ============================================
// Pin Definitions - Rotary Encoder
// ============================================
#define ENCODER_PIN_A   34   // CLK
#define ENCODER_PIN_B   35   // DT
#define ENCODER_PIN_BTN 32   // SW (button press)

// ============================================
// Pin Definitions - Touch (CST816S I2C)
// ============================================
#define TOUCH_SDA  21
#define TOUCH_SCL  22
#define TOUCH_INT  16
#define TOUCH_RST  17

// ============================================
// Pin Definitions - Battery Monitoring
// ============================================
// Connect battery voltage via a voltage divider to this ADC pin
// Typical: 100K + 100K divider = reads half of battery voltage
// LiPo range: 3.0V (empty) to 4.2V (full)
// With 1:1 divider: ADC reads 1.5V to 2.1V
#define BATTERY_ADC_PIN 36       // GPIO36 / VP (ADC1_CH0)
#define BATTERY_DIVIDER_RATIO 2.0  // Voltage divider ratio (R1+R2)/R2
#define BATTERY_FULL_V 4.20      // Fully charged LiPo voltage
#define BATTERY_EMPTY_V 3.20     // Considered empty (protect the cell)
#define BATTERY_READ_INTERVAL_MS 30000  // Read battery every 30s

// ============================================
// Backlight / Brightness
// ============================================
// PWM control for display backlight
#define BRIGHTNESS_MIN 10        // Minimum brightness (0-255)
#define BRIGHTNESS_MAX 255       // Maximum brightness
#define BRIGHTNESS_DEFAULT 200   // Default brightness
#define BRIGHTNESS_STEP 25       // Step per encoder click when adjusting
#define LONG_PRESS_MS 800        // Hold button this long to enter brightness mode

// ============================================
// NTP Time Configuration
// ============================================
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 0         // UK = 0 (GMT)
#define DAYLIGHT_OFFSET_SEC 3600 // BST = +1hr in summer

// ============================================
// Preferences storage (NVS Flash)
// ============================================
#define PREFS_NAMESPACE "flightradar"

#endif // CONFIG_H
