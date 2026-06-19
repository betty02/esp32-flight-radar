# ✈️ ESP32 Flight Radar

A live aircraft tracker built on an ESP32 with a 1.28" round GC9A01 display. Planes flying over your location appear on a radar-style screen in real-time, with altitude colour coding, directional arrows, and callsign labels. Tap any aircraft to see flight details including route, airline, and aircraft photo.

```
         ╭──────────────╮
         │   ·  N  ·    │
         │  ╱  ·  ·  ╲  │
         │ W ·  ✚  · E  │  ← Your location at center
         │  ╲  ▶ ·  ╱  │  ← Aircraft moving SE
         │   ·  S  ·    │
         ╰──────────────╯
          1.28" Round TFT
```

## Features

- **Live aircraft tracking** — real-time ADS-B data from OpenSky Network (free, no API key)
- **Radar display** — range rings, compass markers, directional aircraft icons
- **Altitude colour coding** — orange → yellow → green → cyan → magenta
- **Three pages** — Clock | Radar | Settings, controlled by rotary encoder
- **Tap for details** — touch an aircraft to see route, airline, aircraft type & photo
- **WiFi captive portal** — hold button on boot to configure WiFi from your phone
- **Battery indicator** — optional LiPo monitoring with on-screen icon
- **Brightness control** — long-press encoder to adjust backlight
- **Persistent settings** — all config saved to flash, survives power cycles
- **Dead reckoning** — smooth aircraft movement between API updates
- **Browser simulator** — test everything in your browser before flashing hardware

## Hardware

| Component | Description | Example |
|-----------|-------------|---------|
| ESP32 | Any ESP32 dev board with WiFi | ESP32 DevKit V1, NodeMCU-32S |
| Display | 1.28" GC9A01 240×240 round TFT | [AliExpress](https://www.aliexpress.com/item/1005009774001246.html) |
| Rotary encoder | Built into display module (or external KY-040) | |
| Touch | CST816S (built into touch display variants) | |
| Battery (optional) | 3.7V LiPo + voltage divider | Any 500mAh+ LiPo |
| USB cable | For programming and power | Micro-USB or USB-C |

## Wiring

```
ESP32 DevKit              GC9A01 Display
┌─────────────┐           ┌──────────────┐
│         3V3 ├───────────┤ VCC          │
│         GND ├───────────┤ GND          │
│      GPIO23 ├───────────┤ DIN (MOSI)   │
│      GPIO18 ├───────────┤ CLK (SCLK)   │
│       GPIO5 ├───────────┤ CS           │
│       GPIO2 ├───────────┤ DC           │
│       GPIO4 ├───────────┤ RST          │
│      GPIO15 ├───────────┤ BL           │
├─────────────┤           └──────────────┘
│      GPIO34 ├─── Encoder CLK
│      GPIO35 ├─── Encoder DT
│      GPIO32 ├─── Encoder SW (button)
│      GPIO21 ├─── Touch SDA (if touch variant)
│      GPIO22 ├─── Touch SCL (if touch variant)
│      GPIO36 ├─── Battery voltage divider (optional)
└─────────────┘
```

### Battery Monitoring (Optional)

```
Battery +  ──┬── 100K ──┬── 100K ──┬── GND
             │          │          │
             │       GPIO36        │
             │    (reads half V)   │
             └─────────────────────┘
```

## Quick Start

### Option 1: PlatformIO (Recommended)

```bash
cd firmware/full
pio run --target upload
pio device monitor
```

PlatformIO handles all library installation and display configuration automatically.

### Option 2: Arduino IDE

1. Install ESP32 board support (Board Manager URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`)
2. Install libraries: `TFT_eSPI`, `ArduinoJson`
3. Copy `firmware/basic/User_Setup.h` → your `Arduino/libraries/TFT_eSPI/` folder
4. Open `firmware/full/esp32-flight-radar-full.ino`
5. Edit `config.h` with your WiFi credentials and location
6. Upload to ESP32

### First Boot

1. **Hold the encoder button** while powering on (3 seconds)
2. Connect to WiFi network **"FlightRadar-Setup"** from your phone
3. Open **192.168.4.1** in your browser
4. Enter your WiFi credentials, latitude, longitude, and radius
5. Hit Save — the device restarts and connects to your WiFi

## Browser Simulator

Test the full experience in your browser without any hardware:

```bash
cd simulator
node server.js
# Open http://localhost:8080
```

The simulator includes:
- Identical radar display (canvas-based)
- Live OpenSky API data via proxy
- Map background (OpenStreetMap tiles)
- Click aircraft for detail popup (photo, route, aircraft type)
- UK postcode lookup for easy location setting
- Preset locations (Heathrow, JFK, Dubai, etc.)

## Pages (Rotary Encoder Navigation)

| Scroll Left | Center (Default) | Scroll Right |
|:-----------:|:----------------:|:------------:|
| 🕐 Analogue Clock | ✈️ Live Radar | ⚙️ Settings |
| NTP time sync | Aircraft tracking | WiFi, location, radius |
| Date display | Range rings | UTC offset, brightness |
| Battery level | Callsigns + FL | Persistent to flash |

## Controls

| Action | Effect |
|--------|--------|
| Scroll encoder | Switch pages / navigate settings |
| Short press | Select in settings / force refresh on radar |
| Long press (0.8s) | Brightness adjustment mode |
| Hold on boot (3s) | Enter WiFi captive portal |
| Tap aircraft (touch) | Show flight detail popup |

## Altitude Colour Legend

| Colour | Altitude |
|--------|----------|
| 🟠 Orange | Below 5,000 ft |
| 🟡 Yellow | 5,000 – 15,000 ft |
| 🟢 Green | 15,000 – 30,000 ft |
| 🔵 Cyan | 30,000 – 40,000 ft |
| 🟣 Magenta | Above 40,000 ft |

## Data Sources (All Free, No API Keys)

| API | Data Provided | Rate Limit |
|-----|---------------|------------|
| [OpenSky Network](https://opensky-network.org/) | Live aircraft positions, velocity, altitude | 10s (anonymous) |
| [adsbdb.com](https://adsbdb.com/) | Flight routes (origin/destination airports) | Generous |
| [hexdb.io](https://hexdb.io/) | Aircraft type, registration, operator | Generous |
| [Planespotters.net](https://www.planespotters.net/photo/api) | Aircraft photos | Generous |

## Project Structure

```
esp32-flight-radar/
├── firmware/
│   ├── full/                    ← Recommended: all features
│   │   ├── esp32-flight-radar-full.ino
│   │   ├── config.h
│   │   └── platformio.ini
│   ├── basic/                   ← Minimal: radar only, no touch/encoder
│   │   ├── esp32-flight-radar.ino
│   │   ├── config.h
│   │   ├── User_Setup.h
│   │   └── platformio.ini
│   └── touch/                   ← Touch support, no encoder
│       ├── esp32-flight-radar-touch.ino
│       └── User_Setup.h
├── simulator/                   ← Browser preview
│   ├── index.html
│   └── server.js
└── README.md
```

## Firmware Variants

| Variant | Display | Input | Features |
|---------|---------|-------|----------|
| `full` | GC9A01 round | Rotary encoder + touch | All features, 3 pages, portal, battery |
| `touch` | GC9A01 round | Touch only | Radar + tap detail popup |
| `basic` | GC9A01 round | None | Radar display only |

## Configuration

All settings in `config.h` (or via captive portal for the full version):

```c
#define DEFAULT_WIFI_SSID     "YourNetwork"
#define DEFAULT_WIFI_PASSWORD "YourPassword"
#define DEFAULT_CENTER_LAT    51.4700   // Your latitude
#define DEFAULT_CENTER_LON    -0.4543   // Your longitude
#define DEFAULT_RADIUS_NM     25.0      // Nautical miles to track
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Blank/white screen | Check wiring, especially DC and RST |
| No aircraft shown | Verify lat/lon is correct; check WiFi connection |
| WiFi won't connect | ESP32 = 2.4GHz only; hold button on boot to reconfigure |
| "Rate limited" | Increase poll interval or register free at opensky-network.org |
| Inverted colours | Uncomment `TFT_INVERSION_ON` in User_Setup.h |
| Crash/reboot loop | Reduce MAX_AIRCRAFT in config.h (memory) |
| Touch not working | Check I2C wiring (SDA/SCL); some displays don't have touch |
| No battery icon | Normal if no battery connected (auto-hides) |

## How It Works

1. ESP32 connects to WiFi and syncs time via NTP
2. Every 15 seconds, queries OpenSky Network API with a geographic bounding box
3. Parses JSON response into aircraft array (max 20 tracked)
4. Between API calls, dead-reckons positions using heading + velocity
5. Renders radar-style view: range rings, compass, directional aircraft icons
6. On touch/tap: fetches route (adsbdb), aircraft info (hexdb), photo (planespotters)
7. Rotary encoder switches between clock, radar, and settings pages
8. All settings persist to NVS flash (ESP32's non-volatile storage)

## Contributing

Pull requests welcome! Some ideas:
- ESPHome integration for Home Assistant
- MQTT publishing of tracked aircraft
- Aircraft trail/breadcrumb display
- Sound alerts for low-flying aircraft
- Multi-language support
- Custom watch faces for the clock page

## Credits

- Aircraft data: [OpenSky Network](https://opensky-network.org/)
- Route lookup: [adsbdb](https://adsbdb.com/)
- Aircraft metadata: [hexdb.io](https://hexdb.io/)
- Aircraft photos: [Planespotters.net](https://www.planespotters.net/)
- Display library: [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) by Bodmer
- JSON parsing: [ArduinoJson](https://arduinojson.org/) by Benoit Blanchon
- Map tiles: [OpenStreetMap](https://www.openstreetmap.org/)

## License

MIT — use it however you want.
