# BTC display for ESP32-2424S012C-I-Y(B)

Firmware for a small round ESP32-C3 Bitcoin dashboard. It shows the current BTC price, 24 hour, 30 day and 1 year charts, Fear & Greed index, halving/block data, mempool fees, connection status, cache age and display brightness settings.

## Device

Tested on the black shell `ESP32-2424S012C-I-Y(B)` board, sold as a 1.28 inch capacitive touch circular ESP32 display.

Device listing: [ESP32-C3 1.28 inch circular capacitive touch display](https://www.alibaba.com/product-detail/ESP32-C3-1-28-inch-circular_1601475073494.html)

Known hardware from this build:

- MCU: `ESP32-C3-MINI-1U`
- Display: 240 x 240 round `GC9A01` SPI panel
- Touch: capacitive `CST816` compatible controller on I2C address `0x15` (SDA GPIO 4, SCL GPIO 5, INT GPIO 0, RST GPIO 1)
- Backlight: PWM on GPIO 3
- Display SPI: SCLK GPIO 6, MOSI GPIO 7, DC GPIO 2, CS GPIO 10

## Screens

![Price screen](docs/screenshots/price.svg)
![24 hour chart screen](docs/screenshots/chart.svg)
![30 day chart screen](docs/screenshots/chart-30d.svg)
![Fear and Greed screen](docs/screenshots/fear-greed.svg)
![Halving screen](docs/screenshots/halving.svg)
![Mempool fees screen](docs/screenshots/fees.svg)
![Status screen](docs/screenshots/status.svg)
![Brightness screen](docs/screenshots/brightness.svg)

## Features

- BTC/USDT price from Binance
- 24 hour percent and dollar move
- 24 hour, 30 day and 1 year charts
- Chart refresh intervals: 24H every 5 minutes, 30D every 2 hours, 1Y every 6 hours
- Fear & Greed index
- Current Bitcoin block height and halving countdown
- Mempool fee panel
- Configurable display time by UTC offset
- Cached last data in flash, so the display can show old data after restart or without internet
- Freshness indicator after data becomes older than 10 minutes
- Wi-Fi/status menu with connect, hotspot and offline controls
- Brightness settings with tap and hold controls
- Network backoff to avoid freezing the UI when internet is down

## Wi-Fi setup

The display can be configured in two ways.

### Setup hotspot

If the display cannot connect to Wi-Fi, open the system menu on the display and tap `HOTSPOT` to start the setup hotspot:

- SSID: `BTC-Display-Setup`
- Password: `btcwifi123`
- Browser address: `http://192.168.4.1`

Open the page from a notebook or phone, scan nearby networks, then save the SSID and password. The saved network is stored in the ESP32 flash and takes priority over the compile-time credentials.

After saving Wi-Fi details, the setup page shows live connection status. The setup hotspot stays online while the display is connecting, then turns off a few seconds after a successful connection. If connection fails, the hotspot stays online and the page shows the Wi-Fi status so the network name or password can be corrected.

When the setup hotspot is active, the display shows the hotspot SSID, password and `192.168.4.1` address directly on the screen. Tap `OFFLINE MODE` on the display to hide the hotspot and keep using cached/offline screens. In the status menu, tap `HOTSPOT` to start the setup hotspot again.

The same setup page also lets you choose the display time offset from `UTC -12:00` to `UTC +14:00`.

![Wi-Fi setup page](docs/screenshots/web-setup.png)

### Display Wi-Fi controls

Open the status menu from the left or right edge of the display. When Wi-Fi is not connected, tap the Wi-Fi status or `DETAILS` to open the Wi-Fi detail page.

- `CONNECTING`: the display is trying to connect or waiting for the next retry.
- `HOTSPOT`: starts the setup hotspot so Wi-Fi can be changed from a browser.
- `OFFLINE`: stops Wi-Fi retry attempts and keeps showing cached data.
- `CONNECT`: exits offline mode and tries the saved Wi-Fi again.

### Compile-time fallback

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

The default `platformio.ini` uses `COM1`. Change `upload_port` and `monitor_port` if your board appears on a different serial port.

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

HTTPS is used, but the firmware currently calls `setInsecure()` because certificate handling on this small Arduino build is kept simple.
