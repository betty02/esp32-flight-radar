// ============================================
// TFT_eSPI User Setup for GC9A01 Round Display
// ============================================
// 
// IMPORTANT: Copy this file to your TFT_eSPI library folder:
//   Arduino/libraries/TFT_eSPI/User_Setup.h
//
// Or if using PlatformIO, place in your project and configure
// build flags to include it.
//
// This replaces the default User_Setup.h in the TFT_eSPI library.
// ============================================

// ---- Driver Selection ----
#define GC9A01_DRIVER

// ---- Display dimensions ----
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// ---- ESP32 Pin Definitions ----
// Using standard VSPI pins
#define TFT_MOSI 23    // SDA/DIN pin
#define TFT_SCLK 18    // SCL/CLK pin
#define TFT_CS    5    // Chip select
#define TFT_DC    2    // Data/Command (sometimes labeled A0 or RS)
#define TFT_RST   4    // Reset (connect to 3.3V if not used, but recommended)
#define TFT_BL   15    // Backlight control (optional, connect to 3.3V if not used)

// ---- SPI Frequency ----
// GC9A01 can handle up to 80MHz but 40MHz is safer
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// ---- Misc ----
// #define TFT_INVERSION_ON  // Uncomment if colors look inverted
// #define TFT_RGB_ORDER TFT_BGR  // Uncomment if red/blue are swapped

// ---- Font selection ----
#define LOAD_GLCD   // Font 1: Original Adafruit 8 pixel font
#define LOAD_FONT2  // Font 2: Small 16 pixel high font
#define LOAD_FONT4  // Font 4: Medium 26 pixel high font
#define LOAD_GFXFF  // FreeFonts
#define SMOOTH_FONT
