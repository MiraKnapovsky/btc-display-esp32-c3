#define LGFX_USE_V1

#include <Arduino.h>
#include <HTTPClient.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <math.h>
#include <time.h>

#include "bitcoin_logo.h"
#include "secrets.h"

namespace {

constexpr const char* TICKER_URL =
    "https://api.binance.com/api/v3/ticker/24hr?symbol=BTCUSDT";
constexpr const char* KLINES_URL_PREFIX =
    "https://api.binance.com/api/v3/klines?symbol=BTCUSDT&interval=";
constexpr const char* FEAR_GREED_URL = "https://api.alternative.me/fng/?limit=1";
constexpr const char* BLOCK_HEIGHT_URL =
    "https://mempool.space/api/blocks/tip/height";
constexpr const char* FEES_URL =
    "https://mempool.space/api/v1/fees/recommended";
constexpr const char* TIMEZONE_TZ = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
constexpr const char* NTP_SERVER_2 = "time.google.com";

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 240;
constexpr uint8_t TFT_BL = 3;

constexpr uint8_t TOUCH_SDA = 4;
constexpr uint8_t TOUCH_SCL = 5;
constexpr int8_t TOUCH_INT = 0;
constexpr int8_t TOUCH_RST = 1;

constexpr uint32_t PRICE_REFRESH_MS = 60000;
constexpr uint32_t CHART_REFRESH_MS = 6UL * 60UL * 60UL * 1000UL;
constexpr uint32_t FEAR_GREED_REFRESH_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t BLOCK_REFRESH_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t FEES_REFRESH_MS = 2UL * 60UL * 1000UL;
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 7000;
constexpr uint32_t HTTP_TIMEOUT_MS = 6000;
constexpr uint32_t NETWORK_REQUEST_SPACING_MS = 1500;
constexpr uint32_t NETWORK_FAILURE_BACKOFF_MS = 30000;
constexpr uint32_t CACHE_WRITE_MIN_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t AGE_GREEN_SEC = 10UL * 60UL;
constexpr uint32_t AGE_BLUE_SEC = 60UL * 60UL;
constexpr uint32_t AGE_YELLOW_SEC = 12UL * 60UL * 60UL;
constexpr uint32_t AGE_RED_SEC = 24UL * 60UL * 60UL;
constexpr uint8_t PAGE_COUNT = 6;
constexpr uint16_t MENU_EDGE_W = 54;
constexpr int HALVING_INTERVAL = 210000;
constexpr uint32_t CACHE_VERSION = 2;
constexpr uint8_t STATUS_PAGE_COUNT = 3;
constexpr uint8_t BRIGHTNESS_STEP_PCT = 10;
constexpr uint8_t BRIGHTNESS_MIN_RAW = 20;
constexpr uint32_t BRIGHTNESS_HOLD_FIRST_MS = 450;
constexpr uint32_t BRIGHTNESS_HOLD_REPEAT_MS = 170;

enum class Screen : uint8_t {
  Price = 0,
  Chart30 = 1,
  Chart365 = 2,
  FearGreed = 3,
  Halving = 4,
  Fees = 5,
  Status = 6,
};

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Light_PWM _light;
  lgfx::Bus_SPI _bus;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read = 20000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 6;
      cfg.pin_mosi = 7;
      cfg.pin_miso = -1;
      cfg.pin_dc = 2;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _panel.config();
      cfg.pin_cs = 10;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = SCREEN_W;
      cfg.memory_height = SCREEN_H;
      cfg.panel_width = SCREEN_W;
      cfg.panel_height = SCREEN_H;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }

    {
      auto cfg = _light.config();
      cfg.pin_bl = TFT_BL;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 1;
      _light.config(cfg);
      _panel.setLight(&_light);
    }

    setPanel(&_panel);
  }
};

LGFX display;
WiFiClientSecure secureClient;
Preferences cachePrefs;

Screen currentScreen = Screen::Price;
Screen statusReturnScreen = Screen::Price;
uint32_t nextPriceRefresh = 0;
String lastPrice = "";
String lastStatus = "";
String lastWifiStatus = "WiFi unknown";
uint32_t nextWifiAttemptMs = 0;
uint32_t lastPriceFetchedAt = 0;
uint32_t lastPriceFetchedEpoch = 0;
uint32_t lastPriceCacheSavedAt = 0;
float lastPriceValue = NAN;
float lastPriceChangePct = NAN;
float lastPriceChangeAbs = NAN;
float lastHigh24 = NAN;
float lastLow24 = NAN;
bool lastTickerHasStats = false;

int fearGreedValue = -1;
String fearGreedLabel = "not loaded";
String fearGreedStatus = "not loaded";
uint32_t fearGreedNextAttempt = 0;
uint32_t fearGreedFetchedAt = 0;
uint32_t fearGreedFetchedEpoch = 0;
uint32_t fearGreedCacheSavedAt = 0;

int blockHeight = 0;
String blockStatus = "not loaded";
uint32_t blockNextAttempt = 0;
uint32_t blockFetchedAt = 0;
uint32_t blockFetchedEpoch = 0;
uint32_t blockCacheSavedAt = 0;

int feeFastest = -1;
int feeHalfHour = -1;
int feeHour = -1;
int feeEconomy = -1;
int feeMinimum = -1;
String feesStatus = "not loaded";
uint32_t feesNextAttempt = 0;
uint32_t feesFetchedAt = 0;
uint32_t feesFetchedEpoch = 0;
uint32_t feesCacheSavedAt = 0;

float chart30[30];
size_t chart30Count = 0;
uint32_t chart30FetchedAt = 0;
uint32_t chart30FetchedEpoch = 0;
uint32_t chart30CacheSavedAt = 0;
uint32_t chart30NextAttempt = 0;
String chart30Status = "not loaded";

float chart365[53];
size_t chart365Count = 0;
uint32_t chart365FetchedAt = 0;
uint32_t chart365FetchedEpoch = 0;
uint32_t chart365CacheSavedAt = 0;
uint32_t chart365NextAttempt = 0;
String chart365Status = "not loaded";

bool touchActive = false;
bool swipeConsumed = false;
uint16_t touchStartX = 0;
uint16_t touchStartY = 0;
uint16_t touchLastX = 0;
uint16_t touchLastY = 0;
uint32_t touchStartMs = 0;
uint8_t touchSamples = 0;
uint32_t lastScreenChangeMs = 0;
uint32_t nextPreloadAllowedMs = 0;
uint8_t lastGesture = 0;
bool touchLockedUntilRelease = false;
uint32_t ignoreTouchUntilMs = 0;
bool clockConfigured = false;
int lastClockMinute = -1;
uint32_t nextClockCheckMs = 0;
uint8_t statusPage = 0;
bool manualRefreshBusy = false;
uint8_t displayBrightness = 255;
uint8_t displayBrightnessPct = 100;
int8_t brightnessHoldDirection = 0;
uint32_t nextBrightnessHoldRepeatMs = 0;
uint32_t nextNetworkRequestAllowedMs = 0;
uint32_t internetBackoffUntilMs = 0;

void renderCurrentScreen(bool loading);
void handleTouch();

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return display.color565(r, g, b);
}

uint16_t btcOrange() {
  return rgb(247, 147, 26);
}

uint16_t trendColor(float value) {
  if (isnan(value) || fabs(value) < 0.005f) {
    return btcOrange();
  }
  return value >= 0.0f ? rgb(45, 220, 120) : rgb(238, 76, 76);
}

int brightnessPercent() {
  return displayBrightnessPct;
}

uint8_t brightnessRawFromPercent(uint8_t percent) {
  return static_cast<uint8_t>(
      map(percent, 0, 100, BRIGHTNESS_MIN_RAW, 255));
}

void applyBrightnessPercent(int percent, bool save) {
  displayBrightnessPct = static_cast<uint8_t>(constrain(percent, 0, 100));
  displayBrightness = brightnessRawFromPercent(displayBrightnessPct);
  display.setBrightness(displayBrightness);
  if (save) {
    cachePrefs.putUChar("brightPct", displayBrightnessPct);
    cachePrefs.putUChar("bright", displayBrightness);
  }
}

void adjustBrightness(int deltaPct) {
  int next = constrain(static_cast<int>(displayBrightnessPct) + deltaPct, 0, 100);
  if (next == displayBrightnessPct) {
    return;
  }
  applyBrightnessPercent(next, true);
  renderCurrentScreen(false);
}

void loadBrightnessSetting() {
  if (cachePrefs.isKey("brightPct")) {
    displayBrightnessPct = cachePrefs.getUChar("brightPct", 100);
  } else {
    displayBrightness = cachePrefs.getUChar("bright", 255);
    displayBrightnessPct =
        static_cast<uint8_t>((static_cast<uint16_t>(displayBrightness) * 100U +
                              127U) /
                             255U);
    displayBrightnessPct =
        static_cast<uint8_t>(((displayBrightnessPct + 5) / 10) * 10);
  }
  displayBrightnessPct = constrain(displayBrightnessPct, 0, 100);
  displayBrightness = brightnessRawFromPercent(displayBrightnessPct);
}

void initBoardPower() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);
  delay(5);
  digitalWrite(TOUCH_RST, HIGH);
  delay(50);

  pinMode(TOUCH_INT, INPUT_PULLUP);
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);
}

void drawCenteredText(const String& text, int y, int font, uint16_t color) {
  display.setFont(nullptr);
  display.setTextDatum(middle_center);
  display.setTextColor(color, TFT_BLACK);
  display.drawString(text, SCREEN_W / 2, y, font);
}

void drawBitcoinLogo(int x, int y) {
  bool oldSwap = display.getSwapBytes();
  display.setSwapBytes(true);
  display.pushImage(x, y, BTC_LOGO_W, BTC_LOGO_H, BTC_LOGO_32);
  display.setSwapBytes(oldSwap);
}

void configureClockIfNeeded() {
  if (clockConfigured || WiFi.status() != WL_CONNECTED) {
    return;
  }
  configTzTime(TIMEZONE_TZ, NTP_SERVER_1, NTP_SERVER_2);
  clockConfigured = true;
}

bool readLocalTime(tm& info) {
  configureClockIfNeeded();
  return getLocalTime(&info, 20);
}

String clockText() {
  tm info = {};
  if (!readLocalTime(info)) {
    return "--:--";
  }
  char buffer[6] = {};
  strftime(buffer, sizeof(buffer), "%H:%M", &info);
  return String(buffer);
}

uint32_t currentEpoch() {
  configureClockIfNeeded();
  time_t now = time(nullptr);
  if (now < 1700000000) {
    return 0;
  }
  return static_cast<uint32_t>(now);
}

uint32_t markFetchedNow(uint32_t& fetchedAt, uint32_t& fetchedEpoch) {
  fetchedAt = millis();
  uint32_t epoch = currentEpoch();
  if (epoch > 0) {
    fetchedEpoch = epoch;
  }
  return fetchedEpoch;
}

int32_t dataAgeSeconds(uint32_t fetchedAt, uint32_t fetchedEpoch) {
  uint32_t nowEpoch = currentEpoch();
  if (fetchedEpoch > 0 && nowEpoch >= fetchedEpoch) {
    return static_cast<int32_t>(nowEpoch - fetchedEpoch);
  }
  if (fetchedAt > 0) {
    return static_cast<int32_t>((millis() - fetchedAt) / 1000UL);
  }
  return -1;
}

String ageText(uint32_t fetchedAt, uint32_t fetchedEpoch) {
  int32_t ageSec = dataAgeSeconds(fetchedAt, fetchedEpoch);
  if (ageSec < 0) {
    return fetchedEpoch > 0 ? "cache" : "--";
  }
  if (ageSec < 90) {
    return String(ageSec) + "s";
  }
  uint32_t ageMin = ageSec / 60;
  if (ageMin < 90) {
    return String(ageMin) + "m";
  }
  if (ageMin < 48 * 60) {
    return String(ageMin / 60) + "h";
  }
  return String(ageMin / (24 * 60)) + "d";
}

uint16_t freshnessColor(uint32_t fetchedAt, uint32_t fetchedEpoch) {
  int32_t ageSec = dataAgeSeconds(fetchedAt, fetchedEpoch);
  if (ageSec < 0) {
    return rgb(238, 76, 76);
  }
  if (ageSec < static_cast<int32_t>(AGE_GREEN_SEC)) {
    return rgb(250, 250, 244);
  }
  if (ageSec < static_cast<int32_t>(AGE_BLUE_SEC)) {
    return rgb(45, 220, 120);
  }
  if (ageSec < static_cast<int32_t>(AGE_YELLOW_SEC)) {
    return rgb(75, 160, 255);
  }
  if (ageSec < static_cast<int32_t>(AGE_RED_SEC)) {
    return rgb(235, 205, 70);
  }
  return rgb(238, 76, 76);
}

bool shouldDrawFreshnessDot(uint32_t fetchedAt, uint32_t fetchedEpoch) {
  if (fetchedAt == 0 && fetchedEpoch == 0) {
    return false;
  }
  int32_t ageSec = dataAgeSeconds(fetchedAt, fetchedEpoch);
  return ageSec < 0 || ageSec >= static_cast<int32_t>(AGE_GREEN_SEC);
}

void drawFreshnessDot(int x, int y, uint32_t fetchedAt, uint32_t fetchedEpoch) {
  if (!shouldDrawFreshnessDot(fetchedAt, fetchedEpoch)) {
    return;
  }
  display.fillCircle(x, y, 4, freshnessColor(fetchedAt, fetchedEpoch));
  display.drawCircle(x, y, 5, rgb(55, 49, 39));
}

int currentClockMinute() {
  tm info = {};
  if (!readLocalTime(info)) {
    return -1;
  }
  return info.tm_hour * 60 + info.tm_min;
}

void drawFooterClock(int y, uint32_t fetchedAt = 0, uint32_t fetchedEpoch = 0) {
  display.setTextDatum(middle_center);
  display.setTextColor(btcOrange(), TFT_BLACK);
  display.drawString(clockText(), SCREEN_W / 2, y, 2);
  drawFreshnessDot(222, SCREEN_H / 2, fetchedAt, fetchedEpoch);
}

void drawPageDots() {
  int totalW = (PAGE_COUNT - 1) * 16;
  int startX = SCREEN_W / 2 - totalW / 2;
  for (int i = 0; i < PAGE_COUNT; ++i) {
    int x = startX + i * 16;
    uint16_t color =
        static_cast<int>(currentScreen) == i ? btcOrange() : rgb(55, 49, 39);
    display.fillCircle(x, 229, 3, color);
  }
}

void drawBase() {
  display.fillScreen(TFT_BLACK);
  display.fillCircle(SCREEN_W / 2, SCREEN_H / 2, 118, rgb(8, 9, 10));
  display.drawCircle(SCREEN_W / 2, SCREEN_H / 2, 116, rgb(96, 61, 18));
  display.drawCircle(SCREEN_W / 2, SCREEN_H / 2, 111, rgb(34, 28, 18));
}

String formatWholeDollars(const String& raw) {
  int dot = raw.indexOf('.');
  String whole = dot >= 0 ? raw.substring(0, dot) : raw;
  whole.trim();

  String grouped;
  int digits = 0;
  for (int i = whole.length() - 1; i >= 0; --i) {
    char c = whole[i];
    if (c < '0' || c > '9') {
      continue;
    }
    if (digits > 0 && digits % 3 == 0) {
      grouped = "," + grouped;
    }
    grouped = String(c) + grouped;
    ++digits;
  }
  return grouped;
}

String formatInteger(int value) {
  String raw = String(abs(value));
  String grouped;
  int digits = 0;
  for (int i = raw.length() - 1; i >= 0; --i) {
    if (digits > 0 && digits % 3 == 0) {
      grouped = "," + grouped;
    }
    grouped = String(raw[i]) + grouped;
    ++digits;
  }
  return value < 0 ? "-" + grouped : grouped;
}

String formatDollars(float value) {
  if (isnan(value) || value <= 0) {
    return "--";
  }
  return formatWholeDollars(String(value, 0));
}

String formatSignedPercent(float value) {
  if (isnan(value)) {
    return "--";
  }
  String sign = value > 0.0f ? "+" : "";
  return sign + String(value, 2) + "%";
}

String formatSignedDollars(float value) {
  if (isnan(value)) {
    return "--";
  }
  String sign = value > 0.0f ? "+USD " : "-USD ";
  return sign + formatDollars(fabs(value));
}

float percentChange(float start, float end) {
  if (isnan(start) || isnan(end) || start <= 0.0f) {
    return NAN;
  }
  return ((end - start) / start) * 100.0f;
}

int nextHalvingHeight() {
  if (blockHeight <= 0) {
    return 0;
  }
  return (blockHeight / HALVING_INTERVAL + 1) * HALVING_INTERVAL;
}

int halvingBlocksRemaining() {
  int next = nextHalvingHeight();
  return next > blockHeight ? next - blockHeight : 0;
}

String halvingTimeText(int remainingBlocks) {
  if (remainingBlocks <= 0) {
    return "--";
  }
  int days = (remainingBlocks + 143) / 144;
  if (days >= 730) {
    return "~" + String(days / 365.25f, 1) + " years";
  }
  return "~" + String(days) + " days";
}

String formatFee(int fee) {
  return fee >= 0 ? String(fee) : "--";
}

void drawTrendPill(int x, int y, const String& text, float trend) {
  uint16_t color = trendColor(trend);
  display.fillRoundRect(x - 42, y - 12, 84, 24, 8, rgb(16, 18, 18));
  display.drawRoundRect(x - 42, y - 12, 84, 24, 8, color);
  display.setTextDatum(middle_center);
  display.setTextColor(color, rgb(16, 18, 18));
  display.drawString(text, x, y + 1, 2);
}

void drawMiniMetric(int x, int y, int w, const char* label, const String& value,
                    uint16_t valueColor) {
  display.fillRoundRect(x, y, w, 34, 7, rgb(15, 17, 18));
  display.drawRoundRect(x, y, w, 34, 7, rgb(55, 42, 24));
  display.setTextDatum(middle_center);
  display.setTextColor(rgb(150, 140, 124), rgb(15, 17, 18));
  display.drawString(label, x + w / 2, y + 9, 1);
  display.setTextColor(valueColor, rgb(15, 17, 18));
  display.drawString(value, x + w / 2, y + 23, 1);
}

uint16_t feeColor(int fee) {
  if (fee < 0) {
    return rgb(150, 140, 124);
  }
  if (fee <= 2) {
    return rgb(45, 220, 120);
  }
  if (fee <= 12) {
    return btcOrange();
  }
  return rgb(238, 76, 76);
}

void drawFeeCard(int x, int y, int w, const char* label, int fee) {
  uint16_t color = feeColor(fee);
  display.fillRoundRect(x, y, w, 42, 7, rgb(15, 17, 18));
  display.drawRoundRect(x, y, w, 42, 7, color);
  display.setTextDatum(middle_center);
  display.setTextColor(rgb(150, 140, 124), rgb(15, 17, 18));
  display.drawString(label, x + w / 2, y + 10, 1);
  display.setTextColor(color, rgb(15, 17, 18));
  display.drawString(formatFee(fee), x + w / 2, y + 28, 4);
}

void drawStatusRow(int y, const char* label, const String& value,
                   uint16_t valueColor) {
  display.setTextDatum(middle_left);
  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString(label, 34, y, 1);
  display.setTextDatum(middle_right);
  display.setTextColor(valueColor, TFT_BLACK);
  display.drawString(value, 206, y, 1);
}

void drawStatusAgeRow(int y, const char* label, uint32_t fetchedAt,
                      uint32_t fetchedEpoch) {
  drawFreshnessDot(30, y, fetchedAt, fetchedEpoch);
  display.setTextDatum(middle_left);
  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString(label, 42, y, 1);
  display.setTextDatum(middle_right);
  display.setTextColor(freshnessColor(fetchedAt, fetchedEpoch), TFT_BLACK);
  display.drawString(ageText(fetchedAt, fetchedEpoch), 206, y, 1);
}

void drawRefreshButton(int y) {
  uint16_t color = manualRefreshBusy ? rgb(75, 160, 255) : btcOrange();
  display.fillRoundRect(62, y, 116, 26, 8, rgb(16, 18, 18));
  display.drawRoundRect(62, y, 116, 26, 8, color);
  display.setTextDatum(middle_center);
  display.setTextColor(color, rgb(16, 18, 18));
  display.drawString(manualRefreshBusy ? "UPDATING" : "REFRESH", SCREEN_W / 2,
                     y + 13, 2);
}

void drawBrightnessStatusPage() {
  display.setTextDatum(middle_center);
  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString("brightness", SCREEN_W / 2, 82, 1);
  display.setTextColor(rgb(250, 250, 244), TFT_BLACK);
  display.drawString(String(brightnessPercent()) + "%", SCREEN_W / 2, 114, 6);

  constexpr int barX = 46;
  constexpr int barY = 150;
  constexpr int barW = 148;
  constexpr int barH = 12;
  display.fillRoundRect(barX, barY, barW, barH, 6, rgb(28, 22, 14));
  display.drawRoundRect(barX, barY, barW, barH, 6, rgb(92, 60, 22));
  int fillW =
      map(displayBrightnessPct, 0, 100, 0, static_cast<long>(barW - 2));
  if (fillW > 0) {
    display.fillRoundRect(barX + 1, barY + 1, fillW, barH - 2, 5,
                          btcOrange());
  }

  display.fillRoundRect(42, 184, 58, 34, 8, rgb(16, 18, 18));
  display.drawRoundRect(42, 184, 58, 34, 8, btcOrange());
  display.fillRoundRect(140, 184, 58, 34, 8, rgb(16, 18, 18));
  display.drawRoundRect(140, 184, 58, 34, 8, btcOrange());
  display.setTextDatum(middle_center);
  display.setTextColor(btcOrange(), rgb(16, 18, 18));
  display.drawString("-", 71, 201, 4);
  display.drawString("+", 169, 201, 4);
}

void drawPriceScreen(const String& price, const String& status,
                     bool loading = false) {
  drawBase();
  drawBitcoinLogo(104, 13);
  drawCenteredText("BTC / USDT", 55, 2, btcOrange());

  if (loading) {
    drawCenteredText("Loading", 111, 4, rgb(240, 240, 230));
  } else if (price.length()) {
    drawCenteredText(price, 101, 6, rgb(250, 250, 244));
    if (lastTickerHasStats) {
      drawTrendPill(120, 137, formatSignedPercent(lastPriceChangePct),
                    lastPriceChangePct);
      drawMiniMetric(31, 160, 82, "LOW 24H", "USD " + formatDollars(lastLow24),
                     rgb(238, 76, 76));
      drawMiniMetric(127, 160, 82, "HIGH 24H", "USD " + formatDollars(lastHigh24),
                     rgb(45, 220, 120));
    }
  } else {
    drawCenteredText("no data", 112, 4, rgb(250, 170, 110));
    display.setTextDatum(middle_center);
    display.setTextColor(rgb(135, 150, 150), TFT_BLACK);
    display.drawString(status, SCREEN_W / 2, 171, 1);
  }

  drawFooterClock(211, lastPriceFetchedAt, lastPriceFetchedEpoch);
  drawPageDots();
}

void drawChartScreen(const char* title, const float* data, size_t count,
                     const String& status, uint32_t fetchedAt,
                     uint32_t fetchedEpoch, bool loading = false) {
  drawBase();
  drawCenteredText(title, 27, 2, btcOrange());

  if (loading) {
    drawCenteredText("Loading chart", 112, 4, rgb(240, 240, 230));
    display.setTextDatum(middle_center);
    display.setTextColor(rgb(135, 150, 150), TFT_BLACK);
    display.drawString(status, SCREEN_W / 2, 176, 2);
    drawFooterClock(211, fetchedAt, fetchedEpoch);
    drawPageDots();
    return;
  }

  if (count < 2) {
    drawCenteredText("no chart", 112, 4, rgb(250, 170, 110));
    display.setTextDatum(middle_center);
    display.setTextColor(rgb(135, 150, 150), TFT_BLACK);
    display.drawString(status, SCREEN_W / 2, 176, 2);
    drawFooterClock(211, fetchedAt, fetchedEpoch);
    drawPageDots();
    return;
  }

  float minValue = data[0];
  float maxValue = data[0];
  for (size_t i = 1; i < count; ++i) {
    minValue = min(minValue, data[i]);
    maxValue = max(maxValue, data[i]);
  }
  if (maxValue - minValue < 1.0f) {
    maxValue += 1.0f;
    minValue -= 1.0f;
  }
  float changePct = percentChange(data[0], data[count - 1]);
  float changeAbs = data[count - 1] - data[0];
  uint16_t chartColor = trendColor(changePct);

  constexpr int x0 = 25;
  constexpr int y0 = 65;
  constexpr int w = 190;
  constexpr int h = 90;
  uint16_t grid = rgb(45, 36, 24);

  for (int i = 0; i < 4; ++i) {
    int y = y0 + i * h / 3;
    display.drawFastHLine(x0, y, w, grid);
  }
  display.drawRect(x0, y0, w, h, rgb(92, 60, 22));

  auto mapX = [&](size_t i) -> int {
    return x0 + static_cast<int>((w - 1) * i / (count - 1));
  };
  auto mapY = [&](float value) -> int {
    float unit = (value - minValue) / (maxValue - minValue);
    unit = constrain(unit, 0.0f, 1.0f);
    return y0 + h - 1 - static_cast<int>(unit * (h - 1));
  };

  uint16_t shadow = changePct >= 0.0f ? rgb(18, 82, 42) : rgb(82, 24, 24);
  int lastX = mapX(0);
  int lastY = mapY(data[0]);
  for (size_t i = 1; i < count; ++i) {
    int x = mapX(i);
    int y = mapY(data[i]);
    display.drawLine(lastX, lastY + 1, x, y + 1, shadow);
    display.drawLine(lastX, lastY, x, y, chartColor);
    lastX = x;
    lastY = y;
  }
  display.fillCircle(lastX, lastY, 3, btcOrange());

  drawTrendPill(120, 48, formatSignedPercent(changePct), changePct);
  display.setTextDatum(middle_center);
  display.setTextColor(rgb(250, 250, 244), TFT_BLACK);
  display.drawString(lastPrice.length() ? lastPrice : formatDollars(data[count - 1]),
                     SCREEN_W / 2, 176, 4);
  display.setTextColor(trendColor(changeAbs), TFT_BLACK);
  display.drawString(formatSignedDollars(changeAbs), SCREEN_W / 2, 196, 2);
  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString("MIN " + formatDollars(minValue) + "  MAX " +
                         formatDollars(maxValue),
                     SCREEN_W / 2, 206, 1);
  drawFooterClock(218, fetchedAt, fetchedEpoch);
  drawPageDots();
}

uint16_t fearGreedColor(int value) {
  if (value < 25) {
    return rgb(238, 76, 76);
  }
  if (value < 45) {
    return rgb(247, 147, 26);
  }
  if (value < 55) {
    return rgb(220, 220, 150);
  }
  if (value < 75) {
    return rgb(64, 210, 140);
  }
  return rgb(45, 220, 190);
}

void drawFearGreedScreen(bool loading = false) {
  drawBase();
  drawCenteredText("Fear & Greed", 34, 2, btcOrange());

  if (loading || fearGreedValue < 0) {
    drawCenteredText("Loading", 112, 4, rgb(240, 240, 230));
    display.setTextDatum(middle_center);
    display.setTextColor(rgb(135, 150, 150), TFT_BLACK);
    display.drawString(fearGreedStatus, SCREEN_W / 2, 176, 2);
    drawFooterClock(211, fearGreedFetchedAt, fearGreedFetchedEpoch);
    drawPageDots();
    return;
  }

  constexpr int cx = SCREEN_W / 2;
  constexpr int cy = 156;
  constexpr int outerR = 100;
  constexpr int innerR = 83;

  display.fillArc(cx, cy, outerR, innerR, 180, 216, rgb(238, 76, 76));
  display.fillArc(cx, cy, outerR, innerR, 216, 252, btcOrange());
  display.fillArc(cx, cy, outerR, innerR, 252, 288, rgb(220, 220, 150));
  display.fillArc(cx, cy, outerR, innerR, 288, 324, rgb(64, 210, 140));
  display.fillArc(cx, cy, outerR, innerR, 324, 360, rgb(45, 220, 190));

  float angle = 180.0f + constrain(fearGreedValue, 0, 100) * 180.0f / 100.0f;
  float rad = angle * PI / 180.0f;
  int px = cx + static_cast<int>(cos(rad) * 74.0f);
  int py = cy + static_cast<int>(sin(rad) * 74.0f);
  display.fillTriangle(px, py, px - 8, py + 14, px + 8, py + 14,
                       rgb(165, 170, 178));

  display.setTextDatum(middle_center);
  display.setTextColor(rgb(250, 250, 244), TFT_BLACK);
  display.drawString(String(fearGreedValue), SCREEN_W / 2, 119, 6);
  display.setTextColor(fearGreedColor(fearGreedValue), TFT_BLACK);
  display.drawString(fearGreedLabel, SCREEN_W / 2, 157, 2);
  display.setTextColor(rgb(250, 250, 244), TFT_BLACK);
  display.drawString(lastPrice.length() ? lastPrice : "--", SCREEN_W / 2, 181, 2);
  drawFooterClock(211, fearGreedFetchedAt, fearGreedFetchedEpoch);
  drawPageDots();
}

void drawHalvingScreen(bool loading = false) {
  drawBase();
  drawCenteredText("Halving", 27, 2, btcOrange());
  display.setTextDatum(middle_center);
  display.setTextColor(rgb(250, 250, 244), TFT_BLACK);
  display.drawString(String("BTC ") + (lastPrice.length() ? lastPrice : "--"),
                     SCREEN_W / 2, 47, 1);

  if (loading || blockHeight <= 0) {
    drawCenteredText("Loading blocks", 112, 4, rgb(240, 240, 230));
    display.setTextDatum(middle_center);
    display.setTextColor(rgb(135, 150, 150), TFT_BLACK);
    display.drawString(blockStatus, SCREEN_W / 2, 176, 2);
    drawFooterClock(211, blockFetchedAt, blockFetchedEpoch);
    drawPageDots();
    return;
  }

  int next = nextHalvingHeight();
  int remaining = halvingBlocksRemaining();
  int cycleStart = (blockHeight / HALVING_INTERVAL) * HALVING_INTERVAL;
  int cycleProgress = blockHeight - cycleStart;
  int progressPct = constrain(
      static_cast<int>(cycleProgress * 100L / HALVING_INTERVAL), 0, 100);

  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString("current block", SCREEN_W / 2, 65, 1);
  display.setTextColor(rgb(250, 250, 244), TFT_BLACK);
  display.drawString("#" + formatInteger(blockHeight), SCREEN_W / 2, 86, 4);

  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString("until halving", SCREEN_W / 2, 115, 1);
  display.setTextColor(btcOrange(), TFT_BLACK);
  display.drawString(formatInteger(remaining), SCREEN_W / 2, 137, 4);
  display.setTextColor(rgb(250, 250, 244), TFT_BLACK);
  display.drawString(String("blocks  ") + halvingTimeText(remaining),
                     SCREEN_W / 2, 158, 2);

  constexpr int barX = 43;
  constexpr int barY = 178;
  constexpr int barW = 154;
  constexpr int barH = 10;
  display.fillRoundRect(barX, barY, barW, barH, 5, rgb(28, 22, 14));
  display.drawRoundRect(barX, barY, barW, barH, 5, rgb(92, 60, 22));
  int fillW = constrain(progressPct * (barW - 2) / 100, 0, barW - 2);
  if (fillW > 0) {
    display.fillRoundRect(barX + 1, barY + 1, fillW, barH - 2, 4,
                          btcOrange());
  }

  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString(String(progressPct) + "% epoch", SCREEN_W / 2, 196, 1);
  display.drawString(String("target #") + formatInteger(next), SCREEN_W / 2, 207,
                     1);
  drawFooterClock(218, blockFetchedAt, blockFetchedEpoch);
  drawPageDots();
}

void drawFeesScreen(bool loading = false) {
  drawBase();
  drawCenteredText("Mempool fees", 27, 2, btcOrange());
  display.setTextDatum(middle_center);
  display.setTextColor(rgb(250, 250, 244), TFT_BLACK);
  display.drawString(String("BTC ") + (lastPrice.length() ? lastPrice : "--"),
                     SCREEN_W / 2, 47, 1);

  if (loading || feeFastest < 0) {
    drawCenteredText("Loading fees", 112, 4, rgb(240, 240, 230));
    display.setTextDatum(middle_center);
    display.setTextColor(rgb(135, 150, 150), TFT_BLACK);
    display.drawString(feesStatus, SCREEN_W / 2, 176, 2);
    drawFooterClock(211, feesFetchedAt, feesFetchedEpoch);
    drawPageDots();
    return;
  }

  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString("sat/vB", SCREEN_W / 2, 62, 1);

  drawFeeCard(30, 73, 82, "FAST", feeFastest);
  drawFeeCard(128, 73, 82, "30M", feeHalfHour);
  drawFeeCard(30, 124, 82, "1H", feeHour);
  drawFeeCard(128, 124, 82, "ECO", feeEconomy);

  String pressure = "low";
  if (feeFastest > 12) {
    pressure = "high";
  } else if (feeFastest > 2) {
    pressure = "normal";
  }

  display.setTextDatum(middle_center);
  display.setTextColor(feeColor(feeFastest), TFT_BLACK);
  display.drawString(pressure, SCREEN_W / 2, 181, 2);
  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString(String("minimum ") + formatFee(feeMinimum) + " sat/vB",
                     SCREEN_W / 2, 200, 1);
  drawFooterClock(218, feesFetchedAt, feesFetchedEpoch);
  drawPageDots();
}

void drawStatusScreen() {
  drawBase();
  drawCenteredText("System", 24, 2, btcOrange());
  display.setTextDatum(middle_center);
  display.setTextColor(rgb(150, 140, 124), TFT_BLACK);
  display.drawString(String(statusPage + 1) + "/" + String(STATUS_PAGE_COUNT),
                     SCREEN_W / 2, 39, 1);
  display.setTextDatum(middle_right);
  display.setTextColor(btcOrange(), TFT_BLACK);
  display.drawString(clockText(), 204, 39, 1);

  bool wifiOk = WiFi.status() == WL_CONNECTED;
  uint16_t wifiColor = wifiOk ? rgb(45, 220, 120) : rgb(238, 76, 76);
  if (statusPage < 2) {
    display.fillRoundRect(54, 50, 132, 24, 8, rgb(16, 18, 18));
    display.drawRoundRect(54, 50, 132, 24, 8, wifiColor);
    display.setTextDatum(middle_center);
    display.setTextColor(wifiColor, rgb(16, 18, 18));
    display.drawString(wifiOk ? "WiFi OK" : "WiFi OFF", SCREEN_W / 2, 62, 2);
  }

  if (statusPage == 0) {
    if (wifiOk) {
      drawStatusRow(84, "RSSI", String(WiFi.RSSI()) + " dBm", wifiColor);
      drawStatusRow(100, "IP", WiFi.localIP().toString(), rgb(250, 250, 244));
    } else {
      drawStatusRow(84, "Status", lastWifiStatus, wifiColor);
      drawStatusRow(100, "Retry", "10s", btcOrange());
    }

    display.setTextDatum(middle_center);
    drawRefreshButton(110);
    drawStatusAgeRow(140, "BTC", lastPriceFetchedAt, lastPriceFetchedEpoch);
    drawStatusAgeRow(156, "F&G", fearGreedFetchedAt, fearGreedFetchedEpoch);
    drawStatusAgeRow(172, "BLK", blockFetchedAt, blockFetchedEpoch);
    drawStatusAgeRow(188, "FEE", feesFetchedAt, feesFetchedEpoch);
  } else if (statusPage == 1) {
    drawStatusAgeRow(86, "30D", chart30FetchedAt, chart30FetchedEpoch);
    drawStatusAgeRow(102, "1Y", chart365FetchedAt, chart365FetchedEpoch);
    drawStatusRow(122, "SSID", String(WIFI_SSID), rgb(250, 250, 244));
    drawStatusRow(140, "Price", lastPrice.length() ? lastPrice : "--",
                  rgb(250, 250, 244));
    drawStatusRow(158, "Block",
                  blockHeight > 0 ? formatInteger(blockHeight) : "--",
                  rgb(250, 250, 244));
    drawStatusRow(176, "Fee",
                  feeFastest >= 0 ? String(feeFastest) + " sat/vB" : "--",
                  feeColor(feeFastest));
  } else {
    drawBrightnessStatusPage();
  }
}

void renderCurrentScreen(bool loading = false) {
  switch (currentScreen) {
    case Screen::Price:
      drawPriceScreen(lastPrice, lastStatus, loading);
      break;
    case Screen::Chart30:
      drawChartScreen("BTC/USD 30D", chart30, chart30Count, chart30Status,
                      chart30FetchedAt, chart30FetchedEpoch, loading);
      break;
    case Screen::Chart365:
      drawChartScreen("BTC/USD 1Y", chart365, chart365Count, chart365Status,
                      chart365FetchedAt, chart365FetchedEpoch, loading);
      break;
    case Screen::FearGreed:
      drawFearGreedScreen(loading);
      break;
    case Screen::Halving:
      drawHalvingScreen(loading);
      break;
    case Screen::Fees:
      drawFeesScreen(loading);
      break;
    case Screen::Status:
      drawStatusScreen();
      break;
  }
}

void refreshClockDisplayIfNeeded() {
  if (millis() < nextClockCheckMs) {
    return;
  }
  nextClockCheckMs = millis() + 1000;
  int minute = currentClockMinute();
  if (minute < 0 || minute == lastClockMinute ||
      millis() - lastScreenChangeMs < 350) {
    return;
  }
  lastClockMinute = minute;
  renderCurrentScreen(false);
}

bool extractJsonString(const String& payload, const char* field, String& out) {
  String keyName = String("\"") + field + "\"";
  int key = payload.indexOf(keyName);
  if (key < 0) {
    return false;
  }
  int colon = payload.indexOf(':', key);
  int quote1 = payload.indexOf('"', colon);
  int quote2 = payload.indexOf('"', quote1 + 1);
  if (colon < 0 || quote1 < 0 || quote2 < 0) {
    return false;
  }
  out = payload.substring(quote1 + 1, quote2);
  out.trim();
  return out.length() > 0;
}

bool parseJsonFloat(const String& payload, const char* field, float& out) {
  String raw;
  if (!extractJsonString(payload, field, raw)) {
    return false;
  }
  out = raw.toFloat();
  return true;
}

bool extractJsonNumber(const String& payload, const char* field, String& out) {
  String keyName = String("\"") + field + "\"";
  int key = payload.indexOf(keyName);
  if (key < 0) {
    return false;
  }
  int colon = payload.indexOf(':', key);
  if (colon < 0) {
    return false;
  }
  int start = colon + 1;
  while (start < payload.length() &&
         (payload[start] == ' ' || payload[start] == '\t' ||
          payload[start] == '"')) {
    ++start;
  }
  int end = start;
  while (end < payload.length()) {
    char c = payload[end];
    if ((c >= '0' && c <= '9') || c == '-' || c == '.') {
      ++end;
      continue;
    }
    break;
  }
  out = payload.substring(start, end);
  out.trim();
  return out.length() > 0;
}

bool parseJsonInt(const String& payload, const char* field, int& out) {
  String raw;
  if (!extractJsonNumber(payload, field, raw)) {
    return false;
  }
  out = raw.toInt();
  return true;
}

bool parseTicker(const String& payload, String& outPrice, float& outPriceValue,
                 float& outChangePct, float& outChangeAbs, float& outHigh,
                 float& outLow) {
  String rawPrice;
  if (!extractJsonString(payload, "lastPrice", rawPrice)) {
    return false;
  }
  outPriceValue = rawPrice.toFloat();
  outPrice = formatWholeDollars(rawPrice);
  return outPrice.length() > 0 &&
         parseJsonFloat(payload, "priceChangePercent", outChangePct) &&
         parseJsonFloat(payload, "priceChange", outChangeAbs) &&
         parseJsonFloat(payload, "highPrice", outHigh) &&
         parseJsonFloat(payload, "lowPrice", outLow);
}

bool parseFearGreed(const String& payload, int& outValue, String& outLabel) {
  String rawValue;
  if (!extractJsonString(payload, "value", rawValue) ||
      !extractJsonString(payload, "value_classification", outLabel)) {
    return false;
  }
  outValue = constrain(rawValue.toInt(), 0, 100);
  outLabel.trim();
  return outLabel.length() > 0;
}

bool parseKlineCloses(const String& payload, float* out, size_t capacity,
                      size_t& count) {
  count = 0;
  int depth = 0;
  int quotedField = 0;
  bool inQuote = false;
  int quoteStart = 0;

  for (int i = 0; i < payload.length(); ++i) {
    char c = payload[i];
    if (c == '"' && (i == 0 || payload[i - 1] != '\\')) {
      if (!inQuote) {
        inQuote = true;
        quoteStart = i + 1;
      } else {
        if (depth == 2) {
          ++quotedField;
          if (quotedField == 4 && count < capacity) {
            out[count++] = payload.substring(quoteStart, i).toFloat();
          }
        }
        inQuote = false;
      }
      continue;
    }

    if (inQuote) {
      continue;
    }
    if (c == '[') {
      ++depth;
      if (depth == 2) {
        quotedField = 0;
      }
    } else if (c == ']') {
      --depth;
    }
  }

  return count > 1;
}

bool shouldSaveCache(uint32_t& lastSavedAt, bool force = false) {
  if (!force && lastSavedAt > 0 && millis() - lastSavedAt < CACHE_WRITE_MIN_MS) {
    return false;
  }
  lastSavedAt = millis();
  cachePrefs.putUInt("ver", CACHE_VERSION);
  return true;
}

void saveTickerCache(bool force = false) {
  if (!lastPrice.length() || !shouldSaveCache(lastPriceCacheSavedAt, force)) {
    return;
  }
  cachePrefs.putString("price", lastPrice);
  cachePrefs.putFloat("pval", lastPriceValue);
  cachePrefs.putFloat("pchg", lastPriceChangePct);
  cachePrefs.putFloat("pabs", lastPriceChangeAbs);
  cachePrefs.putFloat("phigh", lastHigh24);
  cachePrefs.putFloat("plow", lastLow24);
  cachePrefs.putBool("pstats", lastTickerHasStats);
  cachePrefs.putUInt("pepoch", lastPriceFetchedEpoch);
}

void saveFearGreedCache(bool force = false) {
  if (fearGreedValue < 0 ||
      !shouldSaveCache(fearGreedCacheSavedAt, force)) {
    return;
  }
  cachePrefs.putInt("fgval", fearGreedValue);
  cachePrefs.putString("fglabel", fearGreedLabel);
  cachePrefs.putUInt("fgepoch", fearGreedFetchedEpoch);
}

void saveBlockCache(bool force = false) {
  if (blockHeight <= 0 || !shouldSaveCache(blockCacheSavedAt, force)) {
    return;
  }
  cachePrefs.putInt("bheight", blockHeight);
  cachePrefs.putUInt("bepoch", blockFetchedEpoch);
}

void saveFeesCache(bool force = false) {
  if (feeFastest < 0 || !shouldSaveCache(feesCacheSavedAt, force)) {
    return;
  }
  cachePrefs.putInt("ffast", feeFastest);
  cachePrefs.putInt("fhalf", feeHalfHour);
  cachePrefs.putInt("fhour", feeHour);
  cachePrefs.putInt("feco", feeEconomy);
  cachePrefs.putInt("fmin", feeMinimum);
  cachePrefs.putUInt("fepoch", feesFetchedEpoch);
}

void saveChart30Cache(bool force = false) {
  if (chart30Count < 2 || !shouldSaveCache(chart30CacheSavedAt, force)) {
    return;
  }
  cachePrefs.putUInt("c30cnt", static_cast<uint32_t>(chart30Count));
  cachePrefs.putUInt("c30epoch", chart30FetchedEpoch);
  cachePrefs.putBytes("c30data", chart30, chart30Count * sizeof(float));
}

void saveChart365Cache(bool force = false) {
  if (chart365Count < 2 || !shouldSaveCache(chart365CacheSavedAt, force)) {
    return;
  }
  cachePrefs.putUInt("c365cnt", static_cast<uint32_t>(chart365Count));
  cachePrefs.putUInt("c365epoch", chart365FetchedEpoch);
  cachePrefs.putBytes("c365data", chart365, chart365Count * sizeof(float));
}

void loadCache() {
  if (cachePrefs.getUInt("ver", 0) != CACHE_VERSION) {
    Serial.println("No compatible cache");
    return;
  }

  lastPrice = cachePrefs.getString("price", "");
  if (lastPrice.length()) {
    lastPriceValue = cachePrefs.getFloat("pval", NAN);
    lastPriceChangePct = cachePrefs.getFloat("pchg", NAN);
    lastPriceChangeAbs = cachePrefs.getFloat("pabs", NAN);
    lastHigh24 = cachePrefs.getFloat("phigh", NAN);
    lastLow24 = cachePrefs.getFloat("plow", NAN);
    lastTickerHasStats = cachePrefs.getBool("pstats", false);
    lastPriceFetchedEpoch = cachePrefs.getUInt("pepoch", 0);
    lastStatus = "cache";
  }

  fearGreedValue = cachePrefs.getInt("fgval", -1);
  if (fearGreedValue >= 0) {
    fearGreedLabel = cachePrefs.getString("fglabel", "cache");
    fearGreedFetchedEpoch = cachePrefs.getUInt("fgepoch", 0);
    fearGreedStatus = "cache";
  }

  blockHeight = cachePrefs.getInt("bheight", 0);
  if (blockHeight > 0) {
    blockFetchedEpoch = cachePrefs.getUInt("bepoch", 0);
    blockStatus = "cache";
  }

  feeFastest = cachePrefs.getInt("ffast", -1);
  if (feeFastest >= 0) {
    feeHalfHour = cachePrefs.getInt("fhalf", -1);
    feeHour = cachePrefs.getInt("fhour", -1);
    feeEconomy = cachePrefs.getInt("feco", -1);
    feeMinimum = cachePrefs.getInt("fmin", -1);
    feesFetchedEpoch = cachePrefs.getUInt("fepoch", 0);
    feesStatus = "cache";
  }

  size_t c30Bytes = cachePrefs.isKey("c30data")
                        ? cachePrefs.getBytesLength("c30data")
                        : 0;
  uint32_t c30Count = cachePrefs.getUInt("c30cnt", 0);
  if (c30Bytes >= 2 * sizeof(float) && c30Count > 1) {
    size_t available = c30Bytes / sizeof(float);
    chart30Count = min(static_cast<size_t>(30),
                       min(static_cast<size_t>(c30Count), available));
    cachePrefs.getBytes("c30data", chart30, chart30Count * sizeof(float));
    chart30FetchedEpoch = cachePrefs.getUInt("c30epoch", 0);
    chart30Status = "cache";
  }

  size_t c365Bytes = cachePrefs.isKey("c365data")
                         ? cachePrefs.getBytesLength("c365data")
                         : 0;
  uint32_t c365Count = cachePrefs.getUInt("c365cnt", 0);
  if (c365Bytes >= 2 * sizeof(float) && c365Count > 1) {
    size_t available = c365Bytes / sizeof(float);
    chart365Count = min(static_cast<size_t>(53),
                        min(static_cast<size_t>(c365Count), available));
    cachePrefs.getBytes("c365data", chart365, chart365Count * sizeof(float));
    chart365FetchedEpoch = cachePrefs.getUInt("c365epoch", 0);
    chart365Status = "cache";
  }

  Serial.printf("Cache loaded: price=%s fg=%d block=%d fee=%d c30=%u c365=%u\n",
                lastPrice.c_str(), fearGreedValue, blockHeight, feeFastest,
                static_cast<unsigned>(chart30Count),
                static_cast<unsigned>(chart365Count));
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID";
    case WL_SCAN_COMPLETED:
      return "SCAN_DONE";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

bool ensureWifi(String& status) {
  if (WiFi.status() == WL_CONNECTED) {
    configureClockIfNeeded();
    status = "WiFi OK";
    lastWifiStatus = status;
    nextWifiAttemptMs = 0;
    return true;
  }

  if (millis() < nextWifiAttemptMs) {
    status = lastWifiStatus;
    return false;
  }
  nextWifiAttemptMs = millis() + WIFI_RETRY_MS;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    handleTouch();
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    configureClockIfNeeded();
    status = "WiFi OK";
    lastWifiStatus = status;
    nextWifiAttemptMs = 0;
    return true;
  }

  wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus == WL_NO_SSID_AVAIL) {
    status = "SSID not found";
  } else {
    status = "WiFi " + String(wifiStatusName(wifiStatus));
  }
  lastWifiStatus = status;
  return false;
}

bool beginHttps(HTTPClient& https, const String& url, String& status) {
  secureClient.stop();
  secureClient.setInsecure();
  https.setConnectTimeout(HTTP_TIMEOUT_MS);
  https.setTimeout(HTTP_TIMEOUT_MS);
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!https.begin(secureClient, url)) {
    status = "HTTPS start error";
    return false;
  }
  return true;
}

bool networkRequestAllowed(bool force = false) {
  return force || millis() >= nextNetworkRequestAllowedMs;
}

void finishNetworkRequest() {
  nextNetworkRequestAllowedMs = millis() + NETWORK_REQUEST_SPACING_MS;
}

bool internetBackoffActive(String& status) {
  if (millis() >= internetBackoffUntilMs) {
    return false;
  }
  status = "internet backoff";
  return true;
}

void noteHttpResult(int code) {
  if (code == HTTP_CODE_OK) {
    internetBackoffUntilMs = 0;
  } else if (code < 0) {
    internetBackoffUntilMs = millis() + NETWORK_FAILURE_BACKOFF_MS;
  }
}

bool fetchBtcTicker(String& outPrice, float& outPriceValue, float& outChangePct,
                    float& outChangeAbs, float& outHigh, float& outLow,
                    String& outStatus) {
  if (!ensureWifi(outStatus)) {
    return false;
  }
  if (internetBackoffActive(outStatus)) {
    return false;
  }

  HTTPClient https;
  if (!beginHttps(https, TICKER_URL, outStatus)) {
    return false;
  }

  int code = https.GET();
  noteHttpResult(code);
  if (code != HTTP_CODE_OK) {
    outStatus = "HTTP " + String(code);
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();

  if (!parseTicker(payload, outPrice, outPriceValue, outChangePct, outChangeAbs,
                   outHigh, outLow)) {
    outStatus = "JSON error";
    return false;
  }

  outStatus = "updated";
  return true;
}

bool fetchKlines(const char* interval, size_t limit, float* out, size_t capacity,
                 size_t& count, String& outStatus) {
  if (!ensureWifi(outStatus)) {
    return false;
  }
  if (internetBackoffActive(outStatus)) {
    return false;
  }

  HTTPClient https;
  String url = String(KLINES_URL_PREFIX) + interval + "&limit=" + String(limit);
  if (!beginHttps(https, url, outStatus)) {
    return false;
  }

  int code = https.GET();
  noteHttpResult(code);
  if (code != HTTP_CODE_OK) {
    outStatus = "HTTP " + String(code);
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();

  if (!parseKlineCloses(payload, out, capacity, count)) {
    outStatus = "chart JSON error";
    return false;
  }

  outStatus = "chart loaded";
  return true;
}

bool fetchFearGreed(int& outValue, String& outLabel, String& outStatus) {
  if (!ensureWifi(outStatus)) {
    return false;
  }
  if (internetBackoffActive(outStatus)) {
    return false;
  }

  HTTPClient https;
  if (!beginHttps(https, FEAR_GREED_URL, outStatus)) {
    return false;
  }

  int code = https.GET();
  noteHttpResult(code);
  if (code != HTTP_CODE_OK) {
    outStatus = "HTTP " + String(code);
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();

  if (!parseFearGreed(payload, outValue, outLabel)) {
    outStatus = "sentiment JSON error";
    return false;
  }

  outStatus = "sentiment loaded";
  return true;
}

bool fetchBlockHeight(int& outHeight, String& outStatus) {
  if (!ensureWifi(outStatus)) {
    return false;
  }
  if (internetBackoffActive(outStatus)) {
    return false;
  }

  HTTPClient https;
  if (!beginHttps(https, BLOCK_HEIGHT_URL, outStatus)) {
    return false;
  }

  int code = https.GET();
  noteHttpResult(code);
  if (code != HTTP_CODE_OK) {
    outStatus = "HTTP " + String(code);
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();
  payload.trim();
  outHeight = payload.toInt();
  if (outHeight <= 0) {
    outStatus = "block JSON error";
    return false;
  }

  outStatus = "block loaded";
  return true;
}

bool fetchFees(int& outFastest, int& outHalfHour, int& outHour,
               int& outEconomy, int& outMinimum, String& outStatus) {
  if (!ensureWifi(outStatus)) {
    return false;
  }
  if (internetBackoffActive(outStatus)) {
    return false;
  }

  HTTPClient https;
  if (!beginHttps(https, FEES_URL, outStatus)) {
    return false;
  }

  int code = https.GET();
  noteHttpResult(code);
  if (code != HTTP_CODE_OK) {
    outStatus = "HTTP " + String(code);
    https.end();
    return false;
  }

  String payload = https.getString();
  https.end();

  if (!parseJsonInt(payload, "fastestFee", outFastest) ||
      !parseJsonInt(payload, "halfHourFee", outHalfHour) ||
      !parseJsonInt(payload, "hourFee", outHour)) {
    outStatus = "fee JSON error";
    return false;
  }
  if (!parseJsonInt(payload, "economyFee", outEconomy)) {
    outEconomy = outHour;
  }
  if (!parseJsonInt(payload, "minimumFee", outMinimum)) {
    outMinimum = min(outEconomy, outHour);
  }

  outStatus = "fees loaded";
  return true;
}

void refreshPriceIfNeeded(bool force = false) {
  if (!force && millis() < nextPriceRefresh && lastPrice.length() > 0) {
    return;
  }
  if (!networkRequestAllowed(force)) {
    return;
  }

  String price;
  String status;
  float priceValue = NAN;
  float changePct = NAN;
  float changeAbs = NAN;
  float high24 = NAN;
  float low24 = NAN;
  bool ok = fetchBtcTicker(price, priceValue, changePct, changeAbs, high24,
                           low24, status);
  finishNetworkRequest();
  if (ok) {
    lastPrice = price;
    lastPriceValue = priceValue;
    lastPriceChangePct = changePct;
    lastPriceChangeAbs = changeAbs;
    lastHigh24 = high24;
    lastLow24 = low24;
    lastTickerHasStats = true;
    markFetchedNow(lastPriceFetchedAt, lastPriceFetchedEpoch);
    lastStatus = status;
    saveTickerCache(force);
    Serial.println("BTCUSDT $" + lastPrice + " " +
                   formatSignedPercent(lastPriceChangePct));
  } else {
    if (!lastPrice.length()) {
      lastTickerHasStats = false;
    }
    lastStatus = status;
    Serial.println("Price update failed: " + status);
  }
  nextPriceRefresh = millis() + (ok ? PRICE_REFRESH_MS : WIFI_RETRY_MS);
  renderCurrentScreen(false);
}

void refreshFearGreedIfNeeded(bool drawLoading, bool force = false) {
  if (!force && millis() < fearGreedNextAttempt && fearGreedValue >= 0) {
    return;
  }
  if (!networkRequestAllowed(force)) {
    return;
  }

  fearGreedStatus = "loading sentiment";
  if (drawLoading && currentScreen == Screen::FearGreed) {
    renderCurrentScreen(true);
  }

  int value = -1;
  String label;
  String status;
  bool ok = fetchFearGreed(value, label, status);
  finishNetworkRequest();
  if (ok) {
    fearGreedValue = value;
    fearGreedLabel = label;
    fearGreedStatus = status;
    markFetchedNow(fearGreedFetchedAt, fearGreedFetchedEpoch);
    fearGreedNextAttempt = millis() + FEAR_GREED_REFRESH_MS;
    saveFearGreedCache(force);
    Serial.printf("Fear & Greed %d %s\n", fearGreedValue,
                  fearGreedLabel.c_str());
  } else {
    fearGreedStatus = status;
    fearGreedNextAttempt = millis() + WIFI_RETRY_MS;
    Serial.println("Fear & Greed failed: " + fearGreedStatus);
  }

  if (currentScreen == Screen::FearGreed || currentScreen == Screen::Status) {
    renderCurrentScreen(false);
  }
}

void refreshBlockHeightIfNeeded(bool drawLoading, bool force = false) {
  if (!force && millis() < blockNextAttempt && blockHeight > 0) {
    return;
  }
  if (!networkRequestAllowed(force)) {
    return;
  }

  blockStatus = "loading block";
  if (drawLoading && currentScreen == Screen::Halving) {
    renderCurrentScreen(true);
  }

  int height = 0;
  String status;
  bool ok = fetchBlockHeight(height, status);
  finishNetworkRequest();
  if (ok) {
    blockHeight = height;
    blockStatus = status;
    markFetchedNow(blockFetchedAt, blockFetchedEpoch);
    blockNextAttempt = millis() + BLOCK_REFRESH_MS;
    saveBlockCache(force);
    Serial.printf("Block height %d, halving in %d blocks\n", blockHeight,
                  halvingBlocksRemaining());
  } else {
    blockStatus = status;
    blockNextAttempt = millis() + WIFI_RETRY_MS;
    Serial.println("Block height failed: " + blockStatus);
  }

  if (currentScreen == Screen::Halving || currentScreen == Screen::Status) {
    renderCurrentScreen(false);
  }
}

void refreshFeesIfNeeded(bool drawLoading, bool force = false) {
  if (!force && millis() < feesNextAttempt && feeFastest >= 0) {
    return;
  }
  if (!networkRequestAllowed(force)) {
    return;
  }

  feesStatus = "loading fees";
  if (drawLoading && currentScreen == Screen::Fees) {
    renderCurrentScreen(true);
  }

  int fastest = -1;
  int halfHour = -1;
  int hour = -1;
  int economy = -1;
  int minimum = -1;
  String status;
  bool ok = fetchFees(fastest, halfHour, hour, economy, minimum, status);
  finishNetworkRequest();
  if (ok) {
    feeFastest = fastest;
    feeHalfHour = halfHour;
    feeHour = hour;
    feeEconomy = economy;
    feeMinimum = minimum;
    feesStatus = status;
    markFetchedNow(feesFetchedAt, feesFetchedEpoch);
    feesNextAttempt = millis() + FEES_REFRESH_MS;
    saveFeesCache(force);
    Serial.printf("Fees %d/%d/%d/%d sat/vB\n", feeFastest, feeHalfHour,
                  feeHour, feeEconomy);
  } else {
    feesStatus = status;
    feesNextAttempt = millis() + WIFI_RETRY_MS;
    Serial.println("Fees failed: " + feesStatus);
  }

  if (currentScreen == Screen::Fees || currentScreen == Screen::Status) {
    renderCurrentScreen(false);
  }
}

bool chartStale(uint32_t fetchedAt, uint32_t fetchedEpoch, size_t count,
                uint32_t nextAttempt, bool force = false) {
  if (force) {
    return true;
  }
  if (count == 0) {
    return true;
  }
  if (millis() < nextAttempt) {
    return false;
  }
  int32_t ageSec = dataAgeSeconds(fetchedAt, fetchedEpoch);
  if (ageSec >= 0) {
    return static_cast<uint32_t>(ageSec) > CHART_REFRESH_MS / 1000UL;
  }
  return false;
}

bool refreshChart30IfNeeded(bool drawLoading, bool force = false) {
  if (!chartStale(chart30FetchedAt, chart30FetchedEpoch, chart30Count,
                  chart30NextAttempt, force)) {
    return false;
  }
  if (!networkRequestAllowed(force)) {
    return false;
  }

  chart30Status = "loading 30D";
  if (drawLoading && currentScreen == Screen::Chart30) {
    renderCurrentScreen(true);
  }
  bool ok = fetchKlines("1d", 30, chart30, 30, chart30Count, chart30Status);
  finishNetworkRequest();
  if (ok) {
    markFetchedNow(chart30FetchedAt, chart30FetchedEpoch);
    chart30NextAttempt = chart30FetchedAt + CHART_REFRESH_MS;
    saveChart30Cache(force);
    Serial.printf("30d chart loaded: %u points\n",
                  static_cast<unsigned>(chart30Count));
  } else {
    chart30NextAttempt = millis() + WIFI_RETRY_MS;
    Serial.println("30d chart failed: " + chart30Status);
  }
  if (currentScreen == Screen::Chart30 || currentScreen == Screen::Status) {
    renderCurrentScreen(false);
  }
  return true;
}

bool refreshChart365IfNeeded(bool drawLoading, bool force = false) {
  if (!chartStale(chart365FetchedAt, chart365FetchedEpoch, chart365Count,
                  chart365NextAttempt, force)) {
    return false;
  }
  if (!networkRequestAllowed(force)) {
    return false;
  }

  chart365Status = "loading 1Y";
  if (drawLoading && currentScreen == Screen::Chart365) {
    renderCurrentScreen(true);
  }
  bool ok = fetchKlines("1w", 53, chart365, 53, chart365Count, chart365Status);
  finishNetworkRequest();
  if (ok) {
    markFetchedNow(chart365FetchedAt, chart365FetchedEpoch);
    chart365NextAttempt = chart365FetchedAt + CHART_REFRESH_MS;
    saveChart365Cache(force);
    Serial.printf("1y chart loaded: %u points\n",
                  static_cast<unsigned>(chart365Count));
  } else {
    chart365NextAttempt = millis() + WIFI_RETRY_MS;
    Serial.println("1y chart failed: " + chart365Status);
  }
  if (currentScreen == Screen::Chart365 || currentScreen == Screen::Status) {
    renderCurrentScreen(false);
  }
  return true;
}

void preloadChartsWhenIdle() {
  if (currentScreen != Screen::Price || touchActive ||
      millis() < nextPreloadAllowedMs ||
      millis() - lastScreenChangeMs < 1500) {
    return;
  }
  if (refreshChart30IfNeeded(false)) {
    nextPreloadAllowedMs = millis() + 1500;
    return;
  }
  if (refreshChart365IfNeeded(false)) {
    nextPreloadAllowedMs = millis() + 1500;
  }
}

void requestManualRefresh() {
  if (manualRefreshBusy) {
    return;
  }
  manualRefreshBusy = true;
  nextWifiAttemptMs = 0;
  nextNetworkRequestAllowedMs = 0;
  internetBackoffUntilMs = 0;
  Serial.println("Manual refresh requested");
  renderCurrentScreen(false);
  refreshPriceIfNeeded(true);
  refreshFearGreedIfNeeded(false, true);
  refreshBlockHeightIfNeeded(false, true);
  refreshFeesIfNeeded(false, true);
  refreshChart30IfNeeded(false, true);
  refreshChart365IfNeeded(false, true);
  manualRefreshBusy = false;
  renderCurrentScreen(false);
}

void animatePageChange(int8_t offset) {
  constexpr int frames = 11;
  constexpr int frameMs = 25;
  for (int i = 1; i <= frames; ++i) {
    int extent = SCREEN_H * i / frames;
    if (offset > 0) {
      int y = SCREEN_H - extent;
      display.fillRect(0, y, SCREEN_W, extent, rgb(8, 9, 10));
      display.drawFastHLine(42, y, 156, btcOrange());
      display.drawFastHLine(58, y + 4, 124, rgb(96, 61, 18));
    } else {
      display.fillRect(0, 0, SCREEN_W, extent, rgb(8, 9, 10));
      display.drawFastHLine(42, extent - 1, 156, btcOrange());
      display.drawFastHLine(58, extent - 5, 124, rgb(96, 61, 18));
    }
    delay(frameMs);
  }
}

void animateMenuChange(bool opening, int8_t side) {
  constexpr int frames = 11;
  constexpr int frameMs = 25;
  bool rightSide = side >= 0;
  for (int i = 1; i <= frames; ++i) {
    int extent = SCREEN_W * i / frames;
    int lineX = 0;
    if (opening && rightSide) {
      int x = SCREEN_W - extent;
      display.fillRect(x, 0, extent, SCREEN_H, rgb(8, 9, 10));
      lineX = x;
    } else if (opening) {
      int x = extent - 1;
      display.fillRect(0, 0, extent, SCREEN_H, rgb(8, 9, 10));
      lineX = x;
    } else if (rightSide) {
      int x = extent - 1;
      display.fillRect(0, 0, extent, SCREEN_H, rgb(8, 9, 10));
      lineX = x;
    } else {
      int x = SCREEN_W - extent;
      display.fillRect(x, 0, extent, SCREEN_H, rgb(8, 9, 10));
      lineX = x;
    }

    display.drawFastVLine(lineX, 42, 156, btcOrange());
    int accentX = rightSide == opening ? lineX + 4 : lineX - 4;
    if (accentX >= 0 && accentX < SCREEN_W) {
      display.drawFastVLine(accentX, 58, 124, rgb(96, 61, 18));
    }
    delay(frameMs);
  }
}

void showScreen(Screen screen, int8_t direction, bool menuTransition = false) {
  if (screen == currentScreen) {
    return;
  }
  lastScreenChangeMs = millis();
  nextPreloadAllowedMs = millis() + 4000;
  if (screen == Screen::Status) {
    statusPage = 0;
  }
  if (menuTransition) {
    animateMenuChange(screen == Screen::Status, direction >= 0 ? 1 : -1);
  } else {
    animatePageChange(direction >= 0 ? 1 : -1);
  }
  currentScreen = screen;
  Serial.printf("Screen changed to %u\n", static_cast<unsigned>(currentScreen));
  renderCurrentScreen(false);
}

void showScreenOffset(int8_t offset) {
  if (offset == 0) {
    return;
  }
  if (currentScreen == Screen::Status) {
    return;
  }
  int next =
      (static_cast<int>(currentScreen) + offset + PAGE_COUNT) % PAGE_COUNT;
  showScreen(static_cast<Screen>(next), offset);
}

void showNextScreen() {
  showScreenOffset(1);
}

void showPreviousScreen() {
  showScreenOffset(-1);
}

int8_t directionFromGesture(uint8_t gesture) {
  switch (gesture) {
    case 0x01:  // CST816 swipe up
    case 0x03:  // CST816 swipe left, matches this board's rotated touch axis
      return 1;
    case 0x02:  // CST816 swipe down
    case 0x04:  // CST816 swipe right, matches this board's rotated touch axis
      return -1;
    default:
      return 0;
  }
}

int8_t directionFromDelta(int dx, int dy) {
  if (abs(dx) <= 45 && abs(dy) <= 45) {
    return 0;
  }
  if (abs(dy) >= abs(dx)) {
    return dy > 0 ? -1 : 1;
  }
  return dx > 0 ? -1 : 1;
}

int8_t directionFromTap(uint16_t y) {
  if (y < 95) {
    return -1;
  }
  if (y > 145) {
    return 1;
  }
  return 0;
}

bool handleStatusScroll(int8_t direction) {
  if (direction == 0) {
    return false;
  }
  uint8_t next = statusPage;
  if (direction > 0 && statusPage + 1 < STATUS_PAGE_COUNT) {
    next = statusPage + 1;
  } else if (direction < 0 && statusPage > 0) {
    next = statusPage - 1;
  }
  if (next != statusPage) {
    statusPage = next;
    renderCurrentScreen(false);
  }
  return true;
}

int8_t brightnessControlDirection(uint16_t x, uint16_t y) {
  if (currentScreen != Screen::Status || statusPage != 2) {
    return 0;
  }
  if (x >= 34 && x <= 108 && y >= 176 && y <= 226) {
    return -1;
  }
  if (x >= 132 && x <= 206 && y >= 176 && y <= 226) {
    return 1;
  }
  return 0;
}

bool handleStatusControlTap(uint16_t x, uint16_t y) {
  int8_t brightnessDirection = brightnessControlDirection(x, y);
  if (brightnessDirection != 0) {
    adjustBrightness(brightnessDirection * BRIGHTNESS_STEP_PCT);
    return true;
  }
  if (statusPage == 0 && x >= 56 && x <= 184 && y >= 102 && y <= 144) {
    requestManualRefresh();
    return true;
  }
  return false;
}

bool handleStatusTap(uint16_t x, uint16_t y) {
  if (handleStatusControlTap(x, y)) {
    return true;
  }
  return handleStatusScroll(directionFromTap(y));
}

bool handleTap(uint16_t x, uint16_t y) {
  if (currentScreen == Screen::Status && handleStatusControlTap(x, y)) {
    return true;
  }

  if (x < MENU_EDGE_W || x > SCREEN_W - MENU_EDGE_W) {
    int8_t side = x < MENU_EDGE_W ? -1 : 1;
    if (currentScreen == Screen::Status) {
      showScreen(statusReturnScreen, side, true);
    } else {
      statusReturnScreen = currentScreen;
      showScreen(Screen::Status, side, true);
    }
    return true;
  }

  if (currentScreen == Screen::Status) {
    return handleStatusTap(x, y);
  }

  int8_t tapDirection = directionFromTap(y);
  if (tapDirection == 0) {
    return false;
  }
  showScreenOffset(tapDirection);
  return true;
}

void lockTouchUntilRelease() {
  touchLockedUntilRelease = true;
  ignoreTouchUntilMs = millis() + 250;
  brightnessHoldDirection = 0;
  touchActive = false;
  touchSamples = 0;
  swipeConsumed = true;
}

bool touchReadBytes(uint8_t reg, uint8_t* out, size_t len) {
  Wire.beginTransmission(0x15);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  size_t read =
      Wire.requestFrom(static_cast<uint8_t>(0x15), static_cast<uint8_t>(len));
  if (read != len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    out[i] = Wire.read();
  }
  return true;
}

void handleTouch() {
  if (millis() < ignoreTouchUntilMs) {
    return;
  }

  uint8_t raw[6] = {};
  bool ok = touchReadBytes(0x01, raw, sizeof(raw));
  if (!ok) {
    return;
  }

  uint8_t gesture = raw[0];
  uint8_t points = raw[1] & 0x0F;
  uint16_t x = ((raw[2] & 0x0F) << 8) | raw[3];
  uint16_t y = ((raw[4] & 0x0F) << 8) | raw[5];
  bool touched = points == 1 && x < SCREEN_W && y < SCREEN_H;

  if (touchLockedUntilRelease) {
    if (!touched) {
      touchLockedUntilRelease = false;
      swipeConsumed = false;
      lastGesture = 0;
    } else {
      return;
    }
  }

  bool newGesture =
      touched && directionFromGesture(gesture) != 0 && gesture != lastGesture;
  if (newGesture) {
    Serial.printf("Touch gesture 0x%02x points=%u x=%u y=%u\n", gesture,
                  points, x, y);
    lastGesture = gesture;
  } else if (gesture == 0) {
    lastGesture = 0;
  }

  if (touched && !touchActive) {
    touchActive = true;
    swipeConsumed = false;
    touchStartX = x;
    touchStartY = y;
    touchLastX = x;
    touchLastY = y;
    touchStartMs = millis();
    touchSamples = 1;

    brightnessHoldDirection = brightnessControlDirection(x, y);
    if (brightnessHoldDirection != 0) {
      adjustBrightness(brightnessHoldDirection * BRIGHTNESS_STEP_PCT);
      nextBrightnessHoldRepeatMs = millis() + BRIGHTNESS_HOLD_FIRST_MS;
      swipeConsumed = true;
    }
    return;
  }

  if (touched) {
    touchLastX = x;
    touchLastY = y;
    if (touchSamples < 255) {
      ++touchSamples;
    }
    if (brightnessHoldDirection != 0) {
      int8_t currentDirection = brightnessControlDirection(x, y);
      if (currentDirection == brightnessHoldDirection) {
        if (millis() >= nextBrightnessHoldRepeatMs) {
          adjustBrightness(brightnessHoldDirection * BRIGHTNESS_STEP_PCT);
          nextBrightnessHoldRepeatMs = millis() + BRIGHTNESS_HOLD_REPEAT_MS;
        }
      } else {
        brightnessHoldDirection = 0;
      }
      return;
    }
    int dx = static_cast<int>(touchLastX) - static_cast<int>(touchStartX);
    int dy = static_cast<int>(touchLastY) - static_cast<int>(touchStartY);
    uint32_t dt = millis() - touchStartMs;
    int8_t deltaDirection = directionFromDelta(dx, dy);
    if (!swipeConsumed && touchSamples >= 2 && deltaDirection != 0 &&
        dt < 1800 &&
        millis() - lastScreenChangeMs > 450) {
      if (currentScreen == Screen::Status) {
        handleStatusScroll(deltaDirection);
      } else {
        showScreenOffset(deltaDirection);
      }
      lockTouchUntilRelease();
    }
    return;
  }

  if (touchActive) {
    int dx = static_cast<int>(touchLastX) - static_cast<int>(touchStartX);
    int dy = static_cast<int>(touchLastY) - static_cast<int>(touchStartY);
    uint32_t dt = millis() - touchStartMs;
    uint8_t samples = touchSamples;
    touchActive = false;
    touchSamples = 0;

    if (brightnessHoldDirection != 0) {
      brightnessHoldDirection = 0;
      swipeConsumed = false;
      return;
    }

    int8_t deltaDirection = directionFromDelta(dx, dy);
    if (!swipeConsumed && samples >= 2 && deltaDirection != 0 && dt < 1800 &&
        millis() - lastScreenChangeMs > 450) {
      if (currentScreen == Screen::Status) {
        handleStatusScroll(deltaDirection);
      } else {
        showScreenOffset(deltaDirection);
      }
      lockTouchUntilRelease();
    } else if (!swipeConsumed && samples >= 2 && abs(dx) <= 28 &&
               abs(dy) <= 28 && dt < 650 &&
               millis() - lastScreenChangeMs > 450) {
      if (handleTap(touchStartX, touchStartY)) {
        lockTouchUntilRelease();
      }
    }
    return;
  }

  // Do not act on a standalone gesture register here. CST816 can keep the last
  // swipe code after release, which looks like repeated scrolls without this.
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("BTC display boot");

  initBoardPower();
  cachePrefs.begin("btcdisp", false);
  loadBrightnessSetting();
  display.init();
  display.initDMA();
  display.setRotation(0);
  applyBrightnessPercent(displayBrightnessPct, false);
  loadCache();

  uint8_t touchChip = 0;
  if (touchReadBytes(0xA7, &touchChip, 1)) {
    Serial.printf("Touch controller detected, chip id 0x%02x\n", touchChip);
  } else {
    Serial.println("Touch controller not detected at 0x15");
  }

  if (lastPrice.length()) {
    renderCurrentScreen(false);
  } else {
    drawPriceScreen("", "connecting WiFi", true);
  }
  String wifiStatus;
  ensureWifi(wifiStatus);
  configureClockIfNeeded();
  lastStatus = WiFi.status() == WL_CONNECTED ? "WiFi connected" : wifiStatus;
  renderCurrentScreen(false);
  refreshPriceIfNeeded();
  refreshFearGreedIfNeeded(false);
  refreshBlockHeightIfNeeded(false);
  refreshFeesIfNeeded(false);
  nextPreloadAllowedMs = millis() + 4000;
}

void loop() {
  handleTouch();
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'n' || c == 'N') {
      showNextScreen();
    }
  }
  refreshPriceIfNeeded();
  refreshFearGreedIfNeeded(currentScreen == Screen::FearGreed);
  refreshBlockHeightIfNeeded(currentScreen == Screen::Halving);
  refreshFeesIfNeeded(currentScreen == Screen::Fees);
  refreshClockDisplayIfNeeded();
  preloadChartsWhenIdle();
  delay(50);
}
