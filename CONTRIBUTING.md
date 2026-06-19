# Contributing

Thanks for your interest in contributing! This project is open to everyone.

## Ways to Contribute

- **Report bugs** — open an issue with steps to reproduce
- **Request features** — describe what you'd like to see
- **Submit PRs** — code, docs, or examples are all welcome
- **Share your build** — post photos/videos of your setup (open an issue or PR to the README)
- **Star the repo** — helps others find it

## Development Setup

### Browser Simulator (quickest way to hack on the UI)

```bash
cd simulator
node server.js
# Open http://localhost:8080
```

### Firmware (PlatformIO)

```bash
cd firmware/full
pio run              # Build
pio run -t upload    # Flash to ESP32
pio device monitor   # Serial output
```

## Code Style

- Arduino/C++: keep it readable, comment non-obvious logic
- HTML/JS: no build tools, keep it simple and self-contained
- Prefer clarity over cleverness

## Pull Requests

1. Fork the repo
2. Create a branch (`git checkout -b feature/my-thing`)
3. Make your changes
4. Test (at minimum, verify the simulator still works)
5. Submit a PR with a clear description

## Ideas for Contributors

- [ ] ESPHome / Home Assistant integration
- [ ] MQTT publishing of tracked aircraft
- [ ] Aircraft trail/breadcrumb display on radar
- [ ] Sound alerts for specific aircraft or low-flyers
- [ ] Multiple display support (e.g. OLED sidebar)
- [ ] 3D printed enclosure designs
- [ ] Different clock face designs
- [ ] Military/interesting aircraft alerts
- [ ] Integration with local ADS-B receiver (dump1090)
- [ ] Offline mode with SDR receiver instead of API
