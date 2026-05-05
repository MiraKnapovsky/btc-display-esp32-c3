# BTC display for ESP32-2424S012C-I-Y(B)

Firmware for a small round ESP32-C3 Bitcoin dashboard. It shows the current BTC price, 30 day and 1 year charts, Fear & Greed index, halving/block data, mempool fees, connection status, cache age and display brightness settings.

## Device

Tested on the black shell `ESP32-2424S012C-I-Y(B)` board, sold as a 1.28 inch capacitive touch circular ESP32 display.

Known hardware from this build:

- MCU: `ESP32-C3-MINI-1U`
- Display: 240 x 240 round `GC9A01` SPI panel
- Touch: capacitive `CST816` compatible controller on I2C address `0x15`
- Backlight: PWM on GPIO 3
- Touch I2C: SDA GPIO 4, SCL GPIO 5
- Touch INT/RST: GPIO 0 / GPIO 1
- Display SPI: SCLK GPIO 6, MOSI GPIO 7, DC GPIO 2, CS GPIO 10

## Screens

![Price screen](docs/screenshots/price.svg)
![Chart screen](docs/screenshots/chart.svg)
![Fear and Greed screen](docs/screenshots/fear-greed.svg)
![Halving screen](docs/screenshots/halving.svg)
![Mempool fees screen](docs/screenshots/fees.svg)
![Status screen](docs/screenshots/status.svg)
![Brightness screen](docs/screenshots/brightness.svg)

## Features

- BTC/USDT price from Binance
- 24 hour percent and dollar move
- 30 day and 1 year charts
- Fear & Greed index
- Current Bitcoin block height and halving countdown
- Mempool fee panel
- Prague time
- Cached last data in flash, so the display can show old data after restart or without internet
- Freshness indicator after data becomes older than 10 minutes
- Wi-Fi/status menu
- Brightness settings with tap and hold controls
- Network backoff to avoid freezing the UI when internet is down

## Wi-Fi setup

The real Wi-Fi credentials are intentionally not committed.

1. Copy `include/secrets.example.h` to `include/secrets.h`.
2. Edit `include/secrets.h`.
3. Use a 2.4 GHz Wi-Fi network.

Example:

```cpp
#pragma once

constexpr const char* WIFI_SSID = "YOUR_WIFI_SSID";
constexpr const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

`include/secrets.h` is ignored by git, so your Wi-Fi password should stay only on your computer.

## Build and upload

This project uses PlatformIO.

```powershell
platformio run -e esp32-c3-devkitm-1
platformio run -e esp32-c3-devkitm-1 -t upload
```

The default `platformio.ini` uses `COM7`. Change `upload_port` and `monitor_port` if your board appears on a different serial port.

Serial monitor:

```powershell
platformio device monitor -e esp32-c3-devkitm-1
```

## Touch controls

- Swipe from bottom to top: next page
- Swipe from top to bottom: previous page
- Tap lower half: next page
- Tap upper half: previous page
- Tap left or right edge: open or leave the status menu
- In the status menu, scroll/tap between status pages
- On brightness page, tap or hold `-` / `+`

## Data sources

- Price and candles: Binance public API
- Fear & Greed: Alternative.me API
- Blocks and fees: mempool.space API

## Notes

The board can be powered from USB-C or the onboard 1S battery connector. This specific closed-shell unit did not expose a usable battery voltage/charge signal in firmware, so battery percentage is not shown.

HTTPS is used, but the firmware currently calls `setInsecure()` because certificate handling on this small Arduino build is kept simple.
