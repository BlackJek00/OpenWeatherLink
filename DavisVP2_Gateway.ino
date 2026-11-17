/*
  DavisVP2_Gateway.ino
  Davis Vantage Pro2 console + WeatherLink USB data logger
     -> ESP32-S3 (USB host, CP210x driver)
     -> WiFi uploads (Weather Underground protocol, Windy API v2, MQTT, HTTP POST JSON) + local JSON API

  Board : ESP32-S3-DevKitC-1, module ESP32-S3-WROOM-1 N16R8
  Core  : arduino-esp32 3.x (ESP-IDF 5.x). No external libraries required.

  Arduino IDE > Tools:
    Board ................ ESP32S3 Dev Module
    USB Mode ............. Hardware CDC and JTAG     (the OTG controller is used as USB HOST)
    USB CDC On Boot ...... Disabled                  (Serial = UART0 = "UART"/"COM" connector)
    Upload Mode .......... UART0 / Hardware CDC
    Flash Size ........... 16MB (128Mb)
    PSRAM ................ OPI PSRAM
    Partition Scheme ..... 16M Flash (3MB APP/9.9MB FATFS)   (any scheme with two OTA app slots)
    Flash and logs ONLY through the "UART" connector: once this firmware runs, the native USB
    PHY belongs to the USB host and the "USB" connector no longer works as a programming port.

  Wiring (the DevKitC-1 does NOT supply 5 V on VBUS of its native USB port):
    USB-A female breakout  ->  DevKitC-1
      VBUS (red)           ->  5V pin   (board powered from the UART connector or a 5 V supply)
      D-   (white)         ->  GPIO19
      D+   (green)         ->  GPIO20
      GND  (black)         ->  GND
    Plug the Davis USB logger cable into the breakout. Do not use the board's "USB" connector at
    the same time (it is wired to the same GPIO19/20). The console must be on its AC adapter.

  Operation:
    - All parameters live in NVS (Preferences, namespace "davis").
    - No SSID configured, or N consecutive failed STA attempts  ->  config AP "DavisVP2-XXXXXX"
      (password default "davisvp2"), captive portal at http://192.168.4.1/config
    - While in AP mode the configured network is retried every "AP retry" seconds when no client
      is connected to the AP; when it comes back the AP closes by itself.
    - BOOT button held 3 s -> open config AP now.  Held 10 s -> factory reset (NVS erase) + reboot.
    - Web UI on STA side: http://<hostname>.local/  user "admin", password default "davisvp2".
      /api/data is public read-only JSON. /update = firmware upload (OTA).
    - RGB LED: blue blink = config AP, yellow blink = connecting, green = online + console OK,
      magenta = USB logger not detected, red = console not answering.
*/

#if !defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(CONFIG_IDF_TARGET_ESP32S2)
#error "This firmware needs an ESP32-S3 or ESP32-S2 (USB OTG host)."
#endif
#if defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 0)
#error "Tools > USB Mode must be 'Hardware CDC and JTAG': the OTG controller is used as USB host."
#endif
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
#error "Tools > USB CDC On Boot must be 'Disabled': logs go to the UART connector."
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_idf_version.h"
#include "esp_arduino_version.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "mqtt_client.h"

// ------------------------------------------------------------------------------------------------
// Compile-time options
// ------------------------------------------------------------------------------------------------
#define FW_VERSION        "1.1.0"
#define RGB_LED_PIN       48          // DevKitC-1 v1.0: GPIO48, v1.1: GPIO38
#define LED_BRIGHTNESS    24          // 0..255
#define BOOT_BTN_PIN      0
#define STA_ATTEMPT_MS    15000UL     // timeout of one STA connection attempt
#define NVS_NS            "davis"
#define KEEP_SENTINEL     "__keep__"  // value shown in password fields that are already set

static const char *DEF_AP_PASS    = "davisvp2";
static const char *DEF_ADMIN_PASS = "davisvp2";
static const char *DEF_HOST       = "davis-vp2";
static const char *DEF_TZ         = "CET-1CEST,M3.5.0,M10.5.0/3";
static const char *DEF_NTP        = "pool.ntp.org";
static const char *DEF_WU_URL     = "https://weatherstation.wunderground.com/weatherstation/updateweatherstation.php";
static const char *DEF_MQ_TOPIC   = "davis/vp2";
static const char *WINDY_URL      = "https://stations.windy.com/api/v2/observation/update";

static constexpr float INHG_TO_HPA = 33.8638866f;
static constexpr float MPH_TO_MS   = 0.44704f;
static constexpr float IN_TO_MM    = 25.4f;

// ------------------------------------------------------------------------------------------------
// Types. All of them stay above the first function: the Arduino builder inserts auto-generated
// prototypes right before the first function definition.
// ------------------------------------------------------------------------------------------------
struct Config {
  // WiFi / network
  String   ssid, pass, host;
  bool     stIp = false;
  String   ip, gw, mask, dns;
  uint16_t maxAtt = 10;        // STA attempts before opening the AP
  uint16_t portRetry = 60;     // seconds between STA retries while the AP is up
  String   apPass, admPass;
  // Davis
  uint32_t baud = 19200;
  uint16_t poll = 5;           // seconds between LOOP2 reads
  uint16_t l1Every = 12;       // read a LOOP (type 0) packet every N cycles
  String   rain = "auto";      // auto | 0.2 | 0.1 | 0.254  (mm per click)
  bool     tSync = true;       // set console clock from NTP once a day
  String   tz, ntp;
  // Derived values: 0 = console value (calculated here if the console does not report it)
  uint8_t  wcMode = 0;         // wind chill: 0 console, 1 calc current wind, 2 calc 2-min avg, 3 calc 10-min avg
  uint8_t  hiMode = 0;         // heat index: 0 console, 1 calculated (NWS)
  uint8_t  dpMode = 0;         // dew point:  0 console, 1 calculated (Magnus)
  // Optional sensors
  bool     sSolar = true;      // solar radiation (also gates THSW and ET)
  bool     sUv = true;         // UV
  bool     sExtra = false;     // extra temperature/humidity stations (7 channels)
  bool     sSoil = false;      // soil/leaf station
  // Weather Underground protocol
  bool     wuEn = false;  String wuUrl, wuId, wuKey;  uint16_t wuInt = 60;
  // Windy API v2
  bool     wyEn = false;  String wyId, wyPass;        uint16_t wyInt = 300;
  // MQTT
  bool     mqEn = false;  String mqHost; uint16_t mqPort = 1883; String mqUser, mqPass, mqTopic; uint16_t mqInt = 10;
  // HTTP POST JSON
  bool     htEn = false;  String htUrl, htTok;        uint16_t htInt = 60;
};

struct Obs {
  bool     valid, hasL2, hasL1;
  uint32_t ms;                                   // millis() at last update
  uint32_t epoch;                                // UTC seconds, 0 if clock not set
  float tOut, tIn, hOut, hIn, dew, heatIdx, windChill, thsw;            // degC, %
  float wind, windAvg2, windAvg10, gust10;                               // m/s
  float windDir, gustDir10;                                              // deg
  float baro, pressAbs, altim, barTrend;                                 // hPa, Davis trend code
  float rainRate, rainDay, rain15, rain1h, rain24, rainStorm, rainMonth, rainYear, etDay;  // mm/h, mm
  float uv, solar, consBattV, txBatt;
  // optional stations, from the LOOP (type 0) packet
  float xT[7], xH[7];                                                    // degC, %
  float soilT[4], soilM[4], leafT[4], leafW[2];                          // degC, centibar, degC, 0..15
};

struct DavisStat {
  volatile uint32_t ok1, ok2, crcErr, badPkt, timeouts, wakeFail, nak;
  volatile uint32_t lastOkMs, lastTimeSyncMs, timeSyncs;
  volatile bool     lps;
  char              rainSrc[24];
  char              lastErr[64];
};

struct UpStat { uint32_t ok = 0, fail = 0, lastMs = 0; int lastCode = 0; };

enum NetState { NET_CONNECTING, NET_ONLINE, NET_PORTAL };

// ------------------------------------------------------------------------------------------------
// Configuration (NVS)
// ------------------------------------------------------------------------------------------------
static Config      cfg;
static Preferences prefs;

static inline float FtoC(float f) { return (f - 32.0f) * 5.0f / 9.0f; }
static inline float CtoF(float c) { return c * 9.0f / 5.0f + 32.0f; }

static String   pS(const char *k, const char *d) { return prefs.isKey(k) ? prefs.getString(k, d) : String(d); }
static uint16_t pU(const char *k, uint16_t d)    { return prefs.isKey(k) ? prefs.getUShort(k, d) : d; }
static uint32_t pL(const char *k, uint32_t d)    { return prefs.isKey(k) ? prefs.getULong(k, d) : d; }
static bool     pB(const char *k, bool d)        { return prefs.isKey(k) ? prefs.getBool(k, d) : d; }
static uint8_t  pC(const char *k, uint8_t d)     { return prefs.isKey(k) ? prefs.getUChar(k, d) : d; }

static uint16_t clampU(uint16_t v, uint16_t lo, uint16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void sanitizeConfig(Config &c) {
  c.host.trim();
  c.host.toLowerCase();
  for (size_t i = 0; i < c.host.length(); i++) {
    char ch = c.host[i];
    if (!isalnum((unsigned char)ch) && ch != '-') c.host.setCharAt(i, '-');
  }
  if (c.host.isEmpty() || c.host.length() > 31) c.host = DEF_HOST;
  if (c.apPass.length() < 8 || c.apPass.length() > 63) c.apPass = DEF_AP_PASS;
  if (c.admPass.length() < 4) c.admPass = DEF_ADMIN_PASS;
  c.maxAtt    = clampU(c.maxAtt, 1, 100);
  c.portRetry = clampU(c.portRetry, 15, 3600);
  c.poll      = clampU(c.poll, 3, 3600);
  c.l1Every   = clampU(c.l1Every, 1, 1000);
  c.wuInt     = clampU(c.wuInt, 3, 3600);
  c.wyInt     = clampU(c.wyInt, 300, 3600);      // Windy: one observation per 5 min max
  c.mqInt     = clampU(c.mqInt, 2, 3600);
  c.htInt     = clampU(c.htInt, 2, 3600);
  if (c.mqPort == 0) c.mqPort = 1883;
  const uint32_t bauds[] = {1200, 2400, 4800, 9600, 14400, 19200};
  bool okBaud = false;
  for (uint32_t b : bauds) if (b == c.baud) okBaud = true;
  if (!okBaud) c.baud = 19200;
  if (c.rain != "auto" && c.rain != "0.2" && c.rain != "0.1" && c.rain != "0.254") c.rain = "auto";
  if (c.wcMode > 3) c.wcMode = 0;
  if (c.hiMode > 1) c.hiMode = 0;
  if (c.dpMode > 1) c.dpMode = 0;
  if (c.tz.isEmpty())  c.tz = DEF_TZ;
  if (c.ntp.isEmpty()) c.ntp = DEF_NTP;
  if (c.wuUrl.isEmpty()) c.wuUrl = DEF_WU_URL;
  if (c.mqTopic.isEmpty()) c.mqTopic = DEF_MQ_TOPIC;
  while (c.mqTopic.endsWith("/")) c.mqTopic.remove(c.mqTopic.length() - 1);
}

static void loadConfig() {
  prefs.begin(NVS_NS, true);
  cfg.ssid = pS("ssid", "");           cfg.pass = pS("pass", "");        cfg.host = pS("host", DEF_HOST);
  cfg.stIp = pB("stip", false);        cfg.ip = pS("ip", "");            cfg.gw = pS("gw", "");
  cfg.mask = pS("mask", "255.255.255.0"); cfg.dns = pS("dns", "");
  cfg.maxAtt = pU("maxatt", 10);       cfg.portRetry = pU("pretry", 60);
  cfg.apPass = pS("appass", DEF_AP_PASS); cfg.admPass = pS("admpass", DEF_ADMIN_PASS);
  cfg.baud = pL("baud", 19200);        cfg.poll = pU("poll", 5);         cfg.l1Every = pU("l1every", 12);
  cfg.rain = pS("rain", "auto");       cfg.tSync = pB("tsync", true);
  cfg.tz = pS("tz", DEF_TZ);           cfg.ntp = pS("ntp", DEF_NTP);
  cfg.wcMode = pC("wcmode", 0);        cfg.hiMode = pC("himode", 0);     cfg.dpMode = pC("dpmode", 0);
  cfg.sSolar = pB("ssolar", true);     cfg.sUv = pB("suv", true);
  cfg.sExtra = pB("sextra", false);    cfg.sSoil = pB("ssoil", false);
  cfg.wuEn = pB("wuen", false);       cfg.wuUrl = pS("wuurl", DEF_WU_URL); cfg.wuId = pS("wuid", "");
  cfg.wuKey = pS("wukey", "");         cfg.wuInt = pU("wuint", 60);
  cfg.wyEn = pB("wyen", false);        cfg.wyId = pS("wyid", "");        cfg.wyPass = pS("wypass", "");
  cfg.wyInt = pU("wyint", 300);
  cfg.mqEn = pB("mqen", false);        cfg.mqHost = pS("mqhost", "");    cfg.mqPort = pU("mqport", 1883);
  cfg.mqUser = pS("mquser", "");       cfg.mqPass = pS("mqpass", "");    cfg.mqTopic = pS("mqtopic", DEF_MQ_TOPIC);
  cfg.mqInt = pU("mqint", 10);
  cfg.htEn = pB("hten", false);        cfg.htUrl = pS("hturl", "");      cfg.htTok = pS("httok", "");
  cfg.htInt = pU("htint", 60);
  prefs.end();
  sanitizeConfig(cfg);
}

static void saveConfig(Config &c) {
  sanitizeConfig(c);
  prefs.begin(NVS_NS, false);
  prefs.putString("ssid", c.ssid);     prefs.putString("pass", c.pass);     prefs.putString("host", c.host);
  prefs.putBool("stip", c.stIp);       prefs.putString("ip", c.ip);         prefs.putString("gw", c.gw);
  prefs.putString("mask", c.mask);     prefs.putString("dns", c.dns);
  prefs.putUShort("maxatt", c.maxAtt); prefs.putUShort("pretry", c.portRetry);
  prefs.putString("appass", c.apPass); prefs.putString("admpass", c.admPass);
  prefs.putULong("baud", c.baud);      prefs.putUShort("poll", c.poll);     prefs.putUShort("l1every", c.l1Every);
  prefs.putString("rain", c.rain);     prefs.putBool("tsync", c.tSync);
  prefs.putString("tz", c.tz);         prefs.putString("ntp", c.ntp);
  prefs.putUChar("wcmode", c.wcMode);  prefs.putUChar("himode", c.hiMode);  prefs.putUChar("dpmode", c.dpMode);
  prefs.putBool("ssolar", c.sSolar);   prefs.putBool("suv", c.sUv);
  prefs.putBool("sextra", c.sExtra);   prefs.putBool("ssoil", c.sSoil);
  prefs.putBool("wuen", c.wuEn);      prefs.putString("wuurl", c.wuUrl);   prefs.putString("wuid", c.wuId);
  prefs.putString("wukey", c.wuKey);   prefs.putUShort("wuint", c.wuInt);
  prefs.putBool("wyen", c.wyEn);       prefs.putString("wyid", c.wyId);     prefs.putString("wypass", c.wyPass);
  prefs.putUShort("wyint", c.wyInt);
  prefs.putBool("mqen", c.mqEn);       prefs.putString("mqhost", c.mqHost); prefs.putUShort("mqport", c.mqPort);
  prefs.putString("mquser", c.mqUser); prefs.putString("mqpass", c.mqPass); prefs.putString("mqtopic", c.mqTopic);
  prefs.putUShort("mqint", c.mqInt);
  prefs.putBool("hten", c.htEn);       prefs.putString("hturl", c.htUrl);   prefs.putString("httok", c.htTok);
  prefs.putUShort("htint", c.htInt);
  prefs.end();
}

static void factoryReset() {
  Serial.println("[CFG] factory reset: erasing NVS namespace and rebooting");
  prefs.begin(NVS_NS, false);
  prefs.clear();
  prefs.end();
  delay(300);
  ESP.restart();
}

// ------------------------------------------------------------------------------------------------
// Observation data
// ------------------------------------------------------------------------------------------------
static void obsReset(Obs &o) {
  memset(&o, 0, sizeof(o));
  float *f[] = {&o.tOut, &o.tIn, &o.hOut, &o.hIn, &o.dew, &o.heatIdx, &o.windChill, &o.thsw,
                &o.wind, &o.windAvg2, &o.windAvg10, &o.gust10, &o.windDir, &o.gustDir10,
                &o.baro, &o.pressAbs, &o.altim, &o.barTrend,
                &o.rainRate, &o.rainDay, &o.rain15, &o.rain1h, &o.rain24, &o.rainStorm, &o.rainMonth, &o.rainYear, &o.etDay,
                &o.uv, &o.solar, &o.consBattV, &o.txBatt};
  for (float *p : f) *p = NAN;
  for (float &v : o.xT) v = NAN;
  for (float &v : o.xH) v = NAN;
  for (float &v : o.soilT) v = NAN;
  for (float &v : o.soilM) v = NAN;
  for (float &v : o.leafT) v = NAN;
  for (float &v : o.leafW) v = NAN;
}

static Obs               gObs;
static SemaphoreHandle_t gObsMtx = nullptr;

static void getObs(Obs &o) {
  xSemaphoreTake(gObsMtx, portMAX_DELAY);
  o = gObs;
  xSemaphoreGive(gObsMtx);
}

static DavisStat     dstat;
static volatile float gClickMm = 0.2f;

static void setErr(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(dstat.lastErr, sizeof(dstat.lastErr), fmt, ap);
  va_end(ap);
  Serial.printf("[DAVIS] %s\n", dstat.lastErr);
}

static bool timeValid() { return time(nullptr) > 1700000000; }

// ------------------------------------------------------------------------------------------------
// USB host: minimal Silicon Labs CP210x driver on top of the ESP-IDF USB Host Library
// ------------------------------------------------------------------------------------------------
static usb_host_client_handle_t gClient = nullptr;
static usb_device_handle_t      gDev    = nullptr;
static usb_transfer_t          *gCtrlX = nullptr, *gInX = nullptr, *gOutX = nullptr;
static uint8_t                  gIf = 0, gEpIn = 0, gEpOut = 0;
static uint16_t                 gMpsIn = 64, gMpsOut = 64;
static volatile uint8_t         gNewAddr = 0;
static volatile bool            gGone = false, gReady = false, gClosing = false;
static volatile bool            gCtrlDone = false, gInFlight = false, gOutFlight = false;
static volatile uint32_t        gUsbGen = 0;
static volatile uint16_t        gVid = 0, gPid = 0;
static SemaphoreHandle_t        gOutSem = nullptr, gOutMtx = nullptr;
static StreamBufferHandle_t     gRx = nullptr;

static void usbClientEvt(const usb_host_client_event_msg_t *m, void *) {
  if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (!gDev) gNewAddr = m->new_dev.address;
  } else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    if (gDev && m->dev_gone.dev_hdl == gDev) gGone = true;
  }
}

static void ctrlCb(usb_transfer_t *) { gCtrlDone = true; }

static void outCb(usb_transfer_t *) {
  gOutFlight = false;
  xSemaphoreGive(gOutSem);
}

static void inCb(usb_transfer_t *x) {
  if (x->status == USB_TRANSFER_STATUS_COMPLETED) {
    if (x->actual_num_bytes > 0) xStreamBufferSend(gRx, x->data_buffer, x->actual_num_bytes, 0);
    if (gReady && !gClosing && usb_host_transfer_submit(x) == ESP_OK) return;   // keep reading
  }
  gInFlight = false;   // client task will resubmit if the device is still there
}

// Pump client events until flag is set (used only from the client task)
static bool pumpUntil(volatile bool &flag, uint32_t ms) {
  uint32_t t0 = millis();
  while (!flag) {
    usb_host_client_handle_events(gClient, pdMS_TO_TICKS(5));
    if (millis() - t0 > ms) return false;
  }
  return true;
}

// Vendor request to interface (bmRequestType 0x41)
static bool cpCtrl(uint8_t req, uint16_t val, const uint8_t *data, uint16_t len) {
  uint8_t *b = gCtrlX->data_buffer;
  b[0] = 0x41; b[1] = req;
  b[2] = val & 0xFF; b[3] = val >> 8;
  b[4] = gIf; b[5] = 0;
  b[6] = len & 0xFF; b[7] = len >> 8;
  if (len) memcpy(b + 8, data, len);
  gCtrlX->num_bytes        = 8 + len;
  gCtrlX->device_handle    = gDev;
  gCtrlX->bEndpointAddress = 0;
  gCtrlX->callback         = ctrlCb;
  gCtrlX->context          = nullptr;
  gCtrlX->timeout_ms       = 1000;
  gCtrlDone = false;
  if (usb_host_transfer_submit_control(gClient, gCtrlX) != ESP_OK) return false;
  if (!pumpUntil(gCtrlDone, 1500)) return false;
  return gCtrlX->status == USB_TRANSFER_STATUS_COMPLETED;
}

static void submitIn() {
  gInX->num_bytes        = gMpsIn;          // multiple of MPS
  gInX->device_handle    = gDev;
  gInX->bEndpointAddress = gEpIn;
  gInX->callback         = inCb;
  gInX->context          = nullptr;
  gInX->timeout_ms       = 0;
  gInFlight = true;
  if (usb_host_transfer_submit(gInX) != ESP_OK) gInFlight = false;
}

static void cpOpen(uint8_t addr) {
  if (usb_host_device_open(gClient, addr, &gDev) != ESP_OK) { gDev = nullptr; return; }

  const usb_device_desc_t *dd = nullptr;
  if (usb_host_get_device_descriptor(gDev, &dd) != ESP_OK || !dd) goto fail_close;
  gVid = dd->idVendor;
  gPid = dd->idProduct;
  Serial.printf("[USB] device %04X:%04X at address %u\n", gVid, gPid, addr);
  if (gVid != 0x10C4) {
    Serial.println("[USB] not a Silicon Labs CP210x bridge, ignored");
    goto fail_close;
  }

  {
    const usb_config_desc_t *cd = nullptr;
    if (usb_host_get_active_config_descriptor(gDev, &cd) != ESP_OK || !cd) goto fail_close;
    const uint8_t *p = (const uint8_t *)cd;
    uint16_t tot = cd->wTotalLength, i = 0;
    bool inIf = false;
    gEpIn = gEpOut = 0;
    while (i + 2 <= tot) {
      uint8_t len = p[i], type = p[i + 1];
      if (len < 2) break;
      if (type == 0x04) {                       // interface descriptor
        if (inIf) break;                        // first interface only
        gIf = p[i + 2];
        inIf = true;
      } else if (type == 0x05 && inIf && i + 6 <= tot) {   // endpoint descriptor
        uint8_t ea = p[i + 2], at = p[i + 3];
        uint16_t mps = p[i + 4] | (p[i + 5] << 8);
        if ((at & 0x03) == 0x02) {              // bulk
          if (ea & 0x80) { gEpIn = ea; gMpsIn = mps ? mps : 64; }
          else           { gEpOut = ea; gMpsOut = mps ? mps : 64; }
        }
      }
      i += len;
    }
    if (!gEpIn || !gEpOut) { Serial.println("[USB] bulk endpoints not found"); goto fail_close; }
    if (gMpsIn > 512) gMpsIn = 512;
    if (gMpsOut > 64) gMpsOut = 64;
  }

  if (usb_host_interface_claim(gClient, gDev, gIf, 0) != ESP_OK) { Serial.println("[USB] claim failed"); goto fail_close; }

  {
    uint32_t bd = cfg.baud;
    uint8_t baud[4] = {(uint8_t)bd, (uint8_t)(bd >> 8), (uint8_t)(bd >> 16), (uint8_t)(bd >> 24)};
    bool ok = cpCtrl(0x00, 0x0001, nullptr, 0)    // IFC_ENABLE
           && cpCtrl(0x03, 0x0800, nullptr, 0)    // SET_LINE_CTL: 8 data, no parity, 1 stop
           && cpCtrl(0x1E, 0x0000, baud, 4)       // SET_BAUDRATE
           && cpCtrl(0x07, 0x0303, nullptr, 0);   // SET_MHS: DTR=1, RTS=1
    if (!ok) {
      Serial.println("[USB] CP210x configuration failed");
      usb_host_interface_release(gClient, gDev, gIf);
      goto fail_close;
    }
  }

  xStreamBufferReset(gRx);
  gReady = true;
  gUsbGen++;
  submitIn();
  Serial.printf("[USB] CP210x ready, %lu 8N1, IF %u, EP IN 0x%02X, EP OUT 0x%02X\n",
                (unsigned long)cfg.baud, gIf, gEpIn, gEpOut);
  return;

fail_close:
  usb_host_device_close(gClient, gDev);
  gDev = nullptr;
}

static void cpClose() {
  if (!gDev) return;
  gReady = false;
  gClosing = true;
  // Hold the OUT mutex so the Davis task cannot submit on a handle being closed. Keep pumping
  // events meanwhile: a pending OUT transfer completes (NO_DEVICE) only through this task.
  bool haveMtx = false;
  uint32_t t0 = millis();
  while (millis() - t0 < 2000) {
    if (xSemaphoreTake(gOutMtx, 0) == pdTRUE) { haveMtx = true; break; }
    usb_host_client_handle_events(gClient, pdMS_TO_TICKS(10));
  }
  t0 = millis();
  while ((gInFlight || gOutFlight) && millis() - t0 < 500) usb_host_client_handle_events(gClient, pdMS_TO_TICKS(10));
  if (gInFlight) {
    usb_host_endpoint_halt(gDev, gEpIn);
    usb_host_endpoint_flush(gDev, gEpIn);
    usb_host_endpoint_clear(gDev, gEpIn);
  }
  if (gOutFlight) {
    usb_host_endpoint_halt(gDev, gEpOut);
    usb_host_endpoint_flush(gDev, gEpOut);
    usb_host_endpoint_clear(gDev, gEpOut);
  }
  t0 = millis();
  while ((gInFlight || gOutFlight) && millis() - t0 < 500) usb_host_client_handle_events(gClient, pdMS_TO_TICKS(10));
  usb_host_interface_release(gClient, gDev, gIf);
  usb_host_device_close(gClient, gDev);
  gDev = nullptr;
  gClosing = false;
  if (haveMtx) xSemaphoreGive(gOutMtx);
  Serial.println("[USB] device removed");
}

static void usbLibTask(void *) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

static void usbClientTask(void *) {
  usb_host_client_config_t cc = {};
  cc.is_synchronous = false;
  cc.max_num_event_msg = 5;
  cc.async.client_event_callback = usbClientEvt;
  cc.async.callback_arg = nullptr;
  if (usb_host_client_register(&cc, &gClient) != ESP_OK) {
    Serial.println("[USB] client register failed");
    vTaskDelete(nullptr);
  }
  usb_host_transfer_alloc(8 + 64, 0, &gCtrlX);
  usb_host_transfer_alloc(512, 0, &gInX);
  usb_host_transfer_alloc(64, 0, &gOutX);
  uint32_t lastInRetry = 0;
  for (;;) {
    usb_host_client_handle_events(gClient, pdMS_TO_TICKS(20));
    if (gGone) { gGone = false; cpClose(); }
    if (gNewAddr && !gDev) { uint8_t a = gNewAddr; gNewAddr = 0; cpOpen(a); }
    if (gReady && !gInFlight && millis() - lastInRetry > 100) { lastInRetry = millis(); submitIn(); }
  }
}

// --- serial-like API used by the Davis task ---
static bool cpWrite(const uint8_t *d, size_t n, uint32_t to) {
  if (!gReady) return false;
  if (xSemaphoreTake(gOutMtx, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
  bool ok = gReady && !gOutFlight;
  while (ok && n) {
    size_t c = n > gMpsOut ? gMpsOut : n;
    memcpy(gOutX->data_buffer, d, c);
    gOutX->num_bytes        = c;
    gOutX->device_handle    = gDev;
    gOutX->bEndpointAddress = gEpOut;
    gOutX->callback         = outCb;
    gOutX->context          = nullptr;
    gOutX->timeout_ms       = to;
    xSemaphoreTake(gOutSem, 0);
    gOutFlight = true;
    if (usb_host_transfer_submit(gOutX) != ESP_OK) { gOutFlight = false; ok = false; break; }
    if (xSemaphoreTake(gOutSem, pdMS_TO_TICKS(to + 200)) != pdTRUE) { ok = false; break; }
    ok = gOutX->status == USB_TRANSFER_STATUS_COMPLETED;
    d += c;
    n -= c;
  }
  xSemaphoreGive(gOutMtx);
  return ok;
}

static size_t rxRead(uint8_t *b, size_t n, uint32_t to) {
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < n) {
    uint32_t el = millis() - t0;
    if (el >= to) break;
    got += xStreamBufferReceive(gRx, b + got, n - got, pdMS_TO_TICKS(to - el));
  }
  return got;
}

static void rxFlush() {
  uint8_t t[64];
  while (xStreamBufferReceive(gRx, t, sizeof(t), 0) > 0) {}
}

// ------------------------------------------------------------------------------------------------
// Davis serial protocol
// ------------------------------------------------------------------------------------------------
static uint16_t crc16(const uint8_t *d, size_t n) {          // CRC-CCITT (XMODEM), as used by Davis
  uint16_t c = 0;
  while (n--) {
    c ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

static bool vpWake() {
  for (int i = 0; i < 3; i++) {
    rxFlush();
    if (!cpWrite((const uint8_t *)"\n", 1, 1000)) return false;
    uint32_t t0 = millis();
    uint8_t prev = 0, c;
    while (millis() - t0 < 1200) {
      if (rxRead(&c, 1, 100) == 1) {
        if (prev == '\n' && c == '\r') return true;
        prev = c;
      }
    }
  }
  return false;
}

// 1 = ACK, -1 = NAK/CANCEL, 0 = timeout
static int waitAck(uint32_t to) {
  uint32_t t0 = millis();
  uint8_t c;
  while (millis() - t0 < to) {
    if (rxRead(&c, 1, 50) == 1) {
      if (c == 0x06) return 1;
      if (c == 0x21 || c == 0x18) return -1;
    }
  }
  return 0;
}

static int vpCmd(const char *cmd, uint32_t to) {
  rxFlush();
  if (!cpWrite((const uint8_t *)cmd, strlen(cmd), 1000)) return 0;
  int r = waitAck(to);
  if (r < 0) dstat.nak++;
  return r;
}

static bool vpReadPacket(uint8_t *b) {
  size_t n = rxRead(b, 99, 3000);
  if (n != 99) { dstat.timeouts++; setErr("packet timeout (%u/99 bytes)", (unsigned)n); return false; }
  if (b[0] != 'L' || b[1] != 'O' || b[2] != 'O' || b[95] != '\n' || b[96] != '\r') {
    dstat.badPkt++; setErr("malformed packet"); return false;
  }
  if (crc16(b, 99) != 0) { dstat.crcErr++; setErr("CRC error"); return false; }
  return true;
}

static bool vpEebrd(uint16_t addr, uint8_t len, uint8_t *out) {
  char cmd[24];
  snprintf(cmd, sizeof(cmd), "EEBRD %02X %02X\n", addr, len);
  if (vpCmd(cmd, 1500) != 1) return false;
  uint8_t buf[66];
  if (len > 64) return false;
  if (rxRead(buf, len + 2, 1500) != (size_t)(len + 2)) return false;
  if (crc16(buf, len + 2) != 0) return false;
  memcpy(out, buf, len);
  return true;
}

static bool vpSetTime() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  uint8_t d[8] = {(uint8_t)lt.tm_sec, (uint8_t)lt.tm_min, (uint8_t)lt.tm_hour,
                  (uint8_t)lt.tm_mday, (uint8_t)(lt.tm_mon + 1), (uint8_t)lt.tm_year};   // year - 1900
  uint16_t c = crc16(d, 6);
  d[6] = c >> 8;          // CRC sent MSB first
  d[7] = c & 0xFF;
  if (!vpWake()) return false;
  if (vpCmd("SETTIME\n", 1500) != 1) return false;
  if (!cpWrite(d, 8, 1000)) return false;
  return waitAck(2000) == 1;
}

// --- packet decoding ---
static inline uint16_t U16(const uint8_t *b, int o) { return (uint16_t)b[o] | ((uint16_t)b[o + 1] << 8); }
static inline int16_t  S16(const uint8_t *b, int o) { return (int16_t)U16(b, o); }
static inline float dTenthF(int16_t v)  { return (v == 32767 || v == -32768) ? NAN : FtoC(v / 10.0f); }
static inline float dWholeF(int16_t v)  { return (v == 255 || v == 32767 || v == -32768) ? NAN : FtoC((float)v); }
static inline float dHum(uint8_t v)     { return (v == 255 || v > 100) ? NAN : (float)v; }
static inline float dBaro(uint16_t v)   { return (v == 0 || v == 0x7FFF || v == 0xFFFF) ? NAN : v / 1000.0f * INHG_TO_HPA; }
static inline float dClicks(uint16_t v) { return v == 0xFFFF ? NAN : v * gClickMm; }
static inline float dDir(uint16_t v)    { return (v == 0 || v > 360) ? NAN : (float)(v % 360); }
static inline float dTrend(uint8_t v)   { return v == 'P' ? NAN : (float)(int8_t)v; }

static float dewPointC(float t, float rh) {
  if (isnan(t) || isnan(rh) || rh <= 0) return NAN;
  const float a = 17.62f, b = 243.12f;
  float g = logf(rh / 100.0f) + a * t / (b + t);
  return b * g / (a - g);
}

// Wind chill, 2001 NWS / Environment Canada formula. T in degC, wind in m/s.
// Defined for T <= 10 degC and wind > 4.8 km/h; outside that range it equals the air temperature.
static float windChillC(float t, float wMs) {
  if (isnan(t) || isnan(wMs)) return NAN;
  float v = wMs * 3.6f;
  if (t > 10.0f || v <= 4.8f) return t;
  float p = powf(v, 0.16f);
  float wc = 13.12f + 0.6215f * t - 11.37f * p + 0.3965f * t * p;
  return wc < t ? wc : t;
}

// Heat index, NWS algorithm (Rothfusz regression with the standard adjustments). T in degC.
static float heatIndexC(float tC, float rh) {
  if (isnan(tC) || isnan(rh)) return NAN;
  float T = CtoF(tC);
  float hi = 0.5f * (T + 61.0f + (T - 68.0f) * 1.2f + rh * 0.094f);
  if ((hi + T) * 0.5f >= 80.0f) {
    hi = -42.379f + 2.04901523f * T + 10.14333127f * rh - 0.22475541f * T * rh
         - 0.00683783f * T * T - 0.05481717f * rh * rh + 0.00122874f * T * T * rh
         + 0.00085282f * T * rh * rh - 0.00000199f * T * T * rh * rh;
    if (rh < 13.0f && T >= 80.0f && T <= 112.0f)
      hi -= ((13.0f - rh) / 4.0f) * sqrtf((17.0f - fabsf(T - 95.0f)) / 17.0f);
    else if (rh > 85.0f && T >= 80.0f && T <= 87.0f)
      hi += ((rh - 85.0f) / 10.0f) * ((87.0f - T) / 5.0f);
  }
  return FtoC(hi);
}

static void parseLoop2(const uint8_t *b, Obs &o) {
  uint16_t v;
  o.barTrend  = dTrend(b[3]);
  o.baro      = dBaro(U16(b, 7));
  o.tIn       = dTenthF(S16(b, 9));
  o.hIn       = dHum(b[11]);
  o.tOut      = dTenthF(S16(b, 12));
  o.wind      = b[14] == 0xFF ? NAN : b[14] * MPH_TO_MS;
  o.windDir   = dDir(U16(b, 16));
  v = U16(b, 18); o.windAvg10 = v == 0xFFFF ? NAN : v / 10.0f * MPH_TO_MS;
  v = U16(b, 20); o.windAvg2  = v == 0xFFFF ? NAN : v / 10.0f * MPH_TO_MS;
  v = U16(b, 22); o.gust10    = (v == 0xFF || v == 0xFFFF) ? NAN : v * MPH_TO_MS;   // whole mph
  v = U16(b, 24); o.gustDir10 = v == 0x7FFF ? NAN : dDir(v);
  o.dew       = dWholeF(S16(b, 30));
  o.hOut      = dHum(b[33]);
  o.heatIdx   = dWholeF(S16(b, 35));
  o.windChill = dWholeF(S16(b, 37));
  o.thsw      = dWholeF(S16(b, 39));
  o.rainRate  = dClicks(U16(b, 41));                 // clicks/h
  o.uv        = b[43] == 0xFF ? NAN : b[43] / 10.0f;
  v = U16(b, 44); o.solar = (v == 0x7FFF || v == 0xFFFF) ? NAN : (float)v;
  o.rainStorm = dClicks(U16(b, 46));
  o.rainDay   = dClicks(U16(b, 50));
  o.rain15    = dClicks(U16(b, 52));
  o.rain1h    = dClicks(U16(b, 54));
  v = U16(b, 56); o.etDay = v == 0xFFFF ? NAN : v / 1000.0f * IN_TO_MM;
  o.rain24    = dClicks(U16(b, 58));
  o.pressAbs  = dBaro(U16(b, 67));
  o.altim     = dBaro(U16(b, 69));
  o.hasL2     = true;
}

static void parseLoop1(const uint8_t *b, Obs &o, bool fillCommon) {
  uint16_t v;
  if (fillCommon) {                                  // only when LOOP2 is not available
    o.barTrend  = dTrend(b[3]);
    o.baro      = dBaro(U16(b, 7));
    o.tIn       = dTenthF(S16(b, 9));
    o.hIn       = dHum(b[11]);
    o.tOut      = dTenthF(S16(b, 12));
    o.wind      = b[14] == 0xFF ? NAN : b[14] * MPH_TO_MS;
    o.windAvg10 = b[15] == 0xFF ? NAN : b[15] * MPH_TO_MS;
    o.windDir   = dDir(U16(b, 16));
    o.hOut      = dHum(b[33]);
    o.rainRate  = dClicks(U16(b, 41));
    o.uv        = b[43] == 0xFF ? NAN : b[43] / 10.0f;
    v = U16(b, 44); o.solar = (v == 0x7FFF || v == 0xFFFF) ? NAN : (float)v;
    o.rainStorm = dClicks(U16(b, 46));
    o.rainDay   = dClicks(U16(b, 50));
    v = U16(b, 56); o.etDay = v == 0xFFFF ? NAN : v / 1000.0f * IN_TO_MM;
    // the LOOP packet has no dew point, heat index, wind chill or THSW: applyDerived() fills them in
    o.dew = o.heatIdx = o.windChill = o.thsw = NAN;
  }
  o.rainMonth = dClicks(U16(b, 52));
  o.rainYear  = dClicks(U16(b, 54));
  o.txBatt    = b[86];
  o.consBattV = ((U16(b, 87) * 300UL) >> 9) / 100.0f;

  // Optional stations. Temperatures: whole degF + 90, 0xFF = no sensor.
  if (cfg.sExtra) {
    for (int i = 0; i < 7; i++) {
      o.xT[i] = b[18 + i] == 0xFF ? NAN : FtoC((float)b[18 + i] - 90.0f);
      o.xH[i] = dHum(b[34 + i]);
    }
  }
  if (cfg.sSoil) {
    for (int i = 0; i < 4; i++) {
      o.soilT[i] = b[25 + i] == 0xFF ? NAN : FtoC((float)b[25 + i] - 90.0f);
      o.leafT[i] = b[29 + i] == 0xFF ? NAN : FtoC((float)b[29 + i] - 90.0f);
      o.soilM[i] = b[62 + i] == 0xFF ? NAN : (float)b[62 + i];                 // centibar
    }
    for (int i = 0; i < 2; i++)                                                 // the console supports 2 leaf wetness inputs
      o.leafW[i] = (b[66 + i] == 0xFF || b[66 + i] > 15) ? NAN : (float)b[66 + i];
  }
  o.hasL1 = true;
}

// Apply the derived-value settings and blank out sensors marked as not installed.
static void applyDerived(Obs &o) {
  if (cfg.dpMode == 1 || isnan(o.dew)) o.dew = dewPointC(o.tOut, o.hOut);
  if (cfg.hiMode == 1 || isnan(o.heatIdx)) o.heatIdx = heatIndexC(o.tOut, o.hOut);
  if (cfg.wcMode != 0 || isnan(o.windChill)) {
    float w = o.wind;
    if (cfg.wcMode == 2 && !isnan(o.windAvg2))  w = o.windAvg2;
    if (cfg.wcMode == 3 && !isnan(o.windAvg10)) w = o.windAvg10;
    o.windChill = windChillC(o.tOut, w);
  }
  if (!cfg.sSolar) { o.solar = NAN; o.thsw = NAN; o.etDay = NAN; }   // THSW and ET need the solar sensor
  if (!cfg.sUv)    o.uv = NAN;
  if (!cfg.sExtra) { for (float &v : o.xT) v = NAN; for (float &v : o.xH) v = NAN; }
  if (!cfg.sSoil)  {
    for (float &v : o.soilT) v = NAN;
    for (float &v : o.soilM) v = NAN;
    for (float &v : o.leafT) v = NAN;
    for (float &v : o.leafW) v = NAN;
  }
}

static void applyRainSetting() {
  if (cfg.rain == "0.2")        { gClickMm = 0.2f;   strlcpy(dstat.rainSrc, "0.2 mm (manual)", sizeof(dstat.rainSrc)); }
  else if (cfg.rain == "0.1")   { gClickMm = 0.1f;   strlcpy(dstat.rainSrc, "0.1 mm (manual)", sizeof(dstat.rainSrc)); }
  else if (cfg.rain == "0.254") { gClickMm = 0.254f; strlcpy(dstat.rainSrc, "0.01 in (manual)", sizeof(dstat.rainSrc)); }
}

static void detectRainCollector() {
  if (cfg.rain != "auto") { applyRainSetting(); return; }
  for (int i = 0; i < 3; i++) {
    uint8_t sb;
    if (vpWake() && vpEebrd(0x2B, 1, &sb)) {           // SETUP_BITS, rain collector in bits 5:4
      uint8_t t = (sb >> 4) & 0x03;
      if (t == 0)      { gClickMm = 0.254f; strlcpy(dstat.rainSrc, "0.01 in (auto)", sizeof(dstat.rainSrc)); }
      else if (t == 1) { gClickMm = 0.2f;   strlcpy(dstat.rainSrc, "0.2 mm (auto)", sizeof(dstat.rainSrc)); }
      else if (t == 2) { gClickMm = 0.1f;   strlcpy(dstat.rainSrc, "0.1 mm (auto)", sizeof(dstat.rainSrc)); }
      else             { gClickMm = 0.2f;   strlcpy(dstat.rainSrc, "0.2 mm (unknown code)", sizeof(dstat.rainSrc)); }
      Serial.printf("[DAVIS] rain collector: %s\n", dstat.rainSrc);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  gClickMm = 0.2f;
  strlcpy(dstat.rainSrc, "0.2 mm (default)", sizeof(dstat.rainSrc));
  setErr("rain collector auto-detect failed, using 0.2 mm");
}

static void davisTask(void *) {
  uint8_t  b[100];
  uint32_t seenGen = 0, cycle = 0;
  uint8_t  lpsNak = 0;
  Obs      w;
  obsReset(w);
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    if (!gReady) {
      vTaskDelay(pdMS_TO_TICKS(500));
      last = xTaskGetTickCount();
      continue;
    }

    if (seenGen != gUsbGen) {                          // freshly (re)connected logger
      seenGen = gUsbGen;
      vTaskDelay(pdMS_TO_TICKS(300));
      dstat.lps = true;
      lpsNak = 0;
      cycle = 0;
      obsReset(w);
      dstat.lastTimeSyncMs = 0;
      detectRainCollector();
      last = xTaskGetTickCount();
    }

    if (cfg.tSync && timeValid() &&
        (dstat.lastTimeSyncMs == 0 || millis() - dstat.lastTimeSyncMs > 86400000UL)) {
      if (vpSetTime()) {
        dstat.lastTimeSyncMs = millis();
        dstat.timeSyncs++;
        Serial.println("[DAVIS] console clock set from NTP");
      } else {
        dstat.lastTimeSyncMs = millis() - 86400000UL + 600000UL;   // retry in 10 min
        if (dstat.lastTimeSyncMs == 0) dstat.lastTimeSyncMs = 1;
        setErr("SETTIME failed");
      }
    }

    bool got = false;
    if (vpWake()) {
      if (dstat.lps) {
        int r = vpCmd("LPS 2 1\n", 1500);
        if (r == 1) {
          if (vpReadPacket(b) && b[4] == 1) { parseLoop2(b, w); dstat.ok2++; got = true; lpsNak = 0; }
        } else if (r < 0 && ++lpsNak >= 3) {
          dstat.lps = false;
          setErr("console rejects LPS (old firmware?), using LOOP only");
        } else if (r == 0) {
          dstat.timeouts++;
          setErr("no ACK to LPS");
        }
      }
      if (!dstat.lps || (cycle % cfg.l1Every) == 0) {
        if (got) vTaskDelay(pdMS_TO_TICKS(50));
        if (vpCmd("LOOP 1\n", 1500) == 1) {
          if (vpReadPacket(b) && b[4] != 1) { parseLoop1(b, w, !dstat.lps); dstat.ok1++; got = true; }
        }
      }
    } else {
      dstat.wakeFail++;
      setErr("console did not answer wake-up");
    }

    if (got) {
      applyDerived(w);
      w.valid = true;
      w.ms    = millis();
      w.epoch = timeValid() ? (uint32_t)time(nullptr) : 0;
      xSemaphoreTake(gObsMtx, portMAX_DELAY);
      gObs = w;
      xSemaphoreGive(gObsMtx);
      dstat.lastOkMs = millis();
    }
    cycle++;
    vTaskDelayUntil(&last, pdMS_TO_TICKS((uint32_t)cfg.poll * 1000UL));
  }
}

static bool davisFresh() {
  return dstat.lastOkMs && (millis() - dstat.lastOkMs) < ((uint32_t)cfg.poll * 3000UL + 10000UL);
}

static String davisState() {
  if (!gReady) return "USB logger not detected";
  if (!dstat.lastOkMs) return "Waiting for console";
  if (!davisFresh()) return "Console not answering (last OK " + String((millis() - dstat.lastOkMs) / 1000) + " s ago)";
  return "OK";
}

// ------------------------------------------------------------------------------------------------
// Helpers (strings, JSON)
// ------------------------------------------------------------------------------------------------
static String gApSsid, gMacSuffix;

static String htmlEsc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  o += F("&amp;");  break;
      case '<':  o += F("&lt;");   break;
      case '>':  o += F("&gt;");   break;
      case '"':  o += F("&quot;"); break;
      case '\'': o += F("&#39;");  break;
      default:   o += c;
    }
  }
  return o;
}

static String jsonEsc(const String &s) {
  String o;
  o.reserve(s.length() + 4);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char t[8]; snprintf(t, sizeof(t), "\\u%04x", c); o += t; }
    else o += c;
  }
  return o;
}

static String urlEnc(const String &s) {
  static const char *hex = "0123456789ABCDEF";
  String o;
  o.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

static void jNum(String &j, const char *k, float v, int dec) {
  j += '"'; j += k; j += F("\":");
  if (isnan(v)) j += F("null"); else j += String(v, dec);
  j += ',';
}

// Optional-station channel: written only when the sensor reports data, e.g. "soil_temp2_c":11.0
static void jChan(String &j, const char *pre, int ch, const char *post, float v, int dec) {
  if (isnan(v)) return;
  j += '"'; j += pre; j += ch; j += post; j += F("\":"); j += String(v, dec); j += ',';
}

static String obsJson(const Obs &o) {
  String j;
  j.reserve(1500);
  j += '{';
  j += F("\"valid\":"); j += o.valid ? F("true,") : F("false,");
  j += F("\"ts\":"); j += o.epoch; j += ',';
  j += F("\"age_s\":"); if (o.valid) j += (millis() - o.ms) / 1000; else j += F("null"); j += ',';
  jNum(j, "temp_out_c", o.tOut, 1);       jNum(j, "temp_in_c", o.tIn, 1);
  jNum(j, "hum_out", o.hOut, 0);          jNum(j, "hum_in", o.hIn, 0);
  jNum(j, "dewpoint_c", o.dew, 1);        jNum(j, "heat_index_c", o.heatIdx, 1);
  jNum(j, "wind_chill_c", o.windChill, 1); jNum(j, "thsw_c", o.thsw, 1);
  jNum(j, "wind_ms", o.wind, 1);          jNum(j, "wind_dir", o.windDir, 0);
  jNum(j, "wind_avg2_ms", o.windAvg2, 1); jNum(j, "wind_avg10_ms", o.windAvg10, 1);
  jNum(j, "gust10_ms", o.gust10, 1);      jNum(j, "gust10_dir", o.gustDir10, 0);
  jNum(j, "baro_hpa", o.baro, 1);         jNum(j, "press_abs_hpa", o.pressAbs, 1);
  jNum(j, "altimeter_hpa", o.altim, 1);   jNum(j, "bar_trend", o.barTrend, 0);
  jNum(j, "rain_rate_mmh", o.rainRate, 1); jNum(j, "rain_day_mm", o.rainDay, 1);
  jNum(j, "rain_15m_mm", o.rain15, 1);    jNum(j, "rain_1h_mm", o.rain1h, 1);
  jNum(j, "rain_24h_mm", o.rain24, 1);    jNum(j, "rain_storm_mm", o.rainStorm, 1);
  jNum(j, "rain_month_mm", o.rainMonth, 1); jNum(j, "rain_year_mm", o.rainYear, 1);
  jNum(j, "et_day_mm", o.etDay, 2);       jNum(j, "uv", o.uv, 1);
  jNum(j, "solar_wm2", o.solar, 0);       jNum(j, "console_batt_v", o.consBattV, 2);
  jNum(j, "tx_batt_status", o.txBatt, 0);
  for (int i = 0; i < 7; i++) { jChan(j, "extra_temp", i + 1, "_c", o.xT[i], 1); jChan(j, "extra_hum", i + 1, "", o.xH[i], 0); }
  for (int i = 0; i < 4; i++) { jChan(j, "soil_temp", i + 1, "_c", o.soilT[i], 1); jChan(j, "soil_moist", i + 1, "_cb", o.soilM[i], 0); }
  for (int i = 0; i < 4; i++) jChan(j, "leaf_temp", i + 1, "_c", o.leafT[i], 1);
  for (int i = 0; i < 2; i++) jChan(j, "leaf_wet", i + 1, "", o.leafW[i], 0);
  j += F("\"loop2\":"); j += o.hasL2 ? F("true") : F("false");
  j += '}';
  return j;
}

// ------------------------------------------------------------------------------------------------
// Uploads
// ------------------------------------------------------------------------------------------------
static UpStat gUpWu, gUpWy, gUpMq, gUpHt;

static void upRecord(UpStat &s, int code, bool ok) {
  s.lastMs = millis();
  s.lastCode = code;
  if (ok) s.ok++; else s.fail++;
}

static int httpReq(const String &url, const String *body, const String &bearer, String *resp) {
  WiFiClient       plain;
  WiFiClientSecure tls;
  tls.setInsecure();                         // no CA bundle on the device; data is not secret
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);
  bool begun = url.startsWith("https://") ? http.begin(tls, url) : http.begin(plain, url);
  if (!begun) return -1000;
  http.setUserAgent("ESP32S3-DavisVP2/" FW_VERSION);
  if (bearer.length()) http.addHeader("Authorization", String("Bearer ") + bearer);
  int code;
  if (body) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(*body);
  } else {
    code = http.GET();
  }
  if (resp && code > 0) *resp = http.getString();
  http.end();
  return code;
}

static void addP(String &u, const char *k, float v, int dec) {
  if (isnan(v)) return;
  u += '&'; u += k; u += '='; u += String(v, dec);
}

static void doWu(const Obs &o) {
  String u = cfg.wuUrl;
  u += (u.indexOf('?') < 0) ? '?' : '&';
  u += "ID=";        u += urlEnc(cfg.wuId);
  u += "&PASSWORD="; u += urlEnc(cfg.wuKey);
  u += "&dateutc=now&action=updateraw&softwaretype=ESP32S3-DavisVP2-" FW_VERSION;
  addP(u, "tempf",            CtoF(o.tOut), 1);
  addP(u, "humidity",         o.hOut, 0);
  addP(u, "dewptf",           CtoF(o.dew), 1);
  addP(u, "winddir",          o.windDir, 0);
  addP(u, "windspeedmph",     o.wind / MPH_TO_MS, 1);
  addP(u, "windgustmph",      (isnan(o.gust10) ? o.wind : o.gust10) / MPH_TO_MS, 1);
  addP(u, "windspdmph_avg2m", o.windAvg2 / MPH_TO_MS, 1);
  addP(u, "windgustmph_10m",  o.gust10 / MPH_TO_MS, 1);
  addP(u, "windgustdir_10m",  o.gustDir10, 0);
  addP(u, "baromin",          o.baro / INHG_TO_HPA, 3);
  addP(u, "rainin",           o.rain1h / IN_TO_MM, 2);
  addP(u, "dailyrainin",      o.rainDay / IN_TO_MM, 2);
  addP(u, "solarradiation",   o.solar, 0);
  addP(u, "UV",               o.uv, 1);
  addP(u, "indoortempf",      CtoF(o.tIn), 1);
  addP(u, "indoorhumidity",   o.hIn, 0);
  String resp;
  int code = httpReq(u, nullptr, "", &resp);
  bool ok = code == 200 && resp.indexOf("success") >= 0;
  upRecord(gUpWu, code, ok);
  if (!ok) { resp.trim(); Serial.printf("[WU] HTTP %d %s\n", code, resp.substring(0, 80).c_str()); }
}

static void doWindy(const Obs &o) {
  String u = WINDY_URL;
  u += "?id=";       u += urlEnc(cfg.wyId);
  u += "&PASSWORD="; u += urlEnc(cfg.wyPass);
  if (timeValid()) {
    time_t now = time(nullptr);
    struct tm g;
    gmtime_r(&now, &g);
    char ts[40];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H%%3A%M%%3A%SZ", &g);      // ISO-8601, ':' url-encoded
    u += "&time="; u += ts;
  }
  addP(u, "temp",           o.tOut, 1);
  addP(u, "dewpoint",       o.dew, 1);
  addP(u, "humidity",       o.hOut, 0);
  addP(u, "wind",           o.wind, 1);
  addP(u, "gust",           isnan(o.gust10) ? o.wind : o.gust10, 1);
  addP(u, "winddir",        o.windDir, 0);
  addP(u, "pressure",       o.baro * 100.0f, 0);                  // Pa
  addP(u, "precip",         o.rain1h, 1);                         // mm, last hour
  addP(u, "uv",             o.uv, 1);
  addP(u, "solarradiation", o.solar, 0);
  String resp;
  int code = httpReq(u, nullptr, "", &resp);
  bool ok = code >= 200 && code < 300;
  upRecord(gUpWy, code, ok);
  if (!ok) { resp.trim(); Serial.printf("[WINDY] HTTP %d %s\n", code, resp.substring(0, 120).c_str()); }
}

static void doHttp(const Obs &o) {
  String body = obsJson(o);
  int code = httpReq(cfg.htUrl, &body, cfg.htTok, nullptr);
  bool ok = code >= 200 && code < 300;
  upRecord(gUpHt, code, ok);
  if (!ok) Serial.printf("[HTTP] POST %d\n", code);
}

// --- MQTT (ESP-IDF esp-mqtt, built into the core) ---
static esp_mqtt_client_handle_t gMqtt = nullptr;
static volatile bool            gMqttUp = false;
static String                   gMqUri, gMqCid, gMqLwt, gMqState;

static void mqttEvt(void *, esp_event_base_t, int32_t id, void *) {
  if (id == MQTT_EVENT_CONNECTED) {
    gMqttUp = true;
    esp_mqtt_client_publish(gMqtt, gMqLwt.c_str(), "online", 0, 1, 1);
    Serial.println("[MQTT] connected");
  } else if (id == MQTT_EVENT_DISCONNECTED) {
    if (gMqttUp) Serial.println("[MQTT] disconnected");
    gMqttUp = false;
  }
}

static void mqttStart() {
  if (!cfg.mqEn || gMqtt || cfg.mqHost.isEmpty()) return;
  if (cfg.mqHost.indexOf("://") >= 0) {
    gMqUri = cfg.mqHost;                                   // full URI given, e.g. mqtt://host:1883
  } else {
    gMqUri = "mqtt://";
    gMqUri += cfg.mqHost;
    gMqUri += ':';
    gMqUri += cfg.mqPort;
  }
  gMqCid   = cfg.host + "-" + gMacSuffix;
  gMqLwt   = cfg.mqTopic + "/status";
  gMqState = cfg.mqTopic + "/state";
  esp_mqtt_client_config_t mc = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  mc.broker.address.uri = gMqUri.c_str();
  mc.credentials.client_id = gMqCid.c_str();
  if (cfg.mqUser.length()) mc.credentials.username = cfg.mqUser.c_str();
  if (cfg.mqPass.length()) mc.credentials.authentication.password = cfg.mqPass.c_str();
  mc.session.keepalive = 30;
  mc.session.last_will.topic = gMqLwt.c_str();
  mc.session.last_will.msg = "offline";
  mc.session.last_will.qos = 1;
  mc.session.last_will.retain = 1;
  mc.network.reconnect_timeout_ms = 10000;
#else
  mc.uri = gMqUri.c_str();
  mc.client_id = gMqCid.c_str();
  if (cfg.mqUser.length()) mc.username = cfg.mqUser.c_str();
  if (cfg.mqPass.length()) mc.password = cfg.mqPass.c_str();
  mc.keepalive = 30;
  mc.lwt_topic = gMqLwt.c_str();
  mc.lwt_msg = "offline";
  mc.lwt_qos = 1;
  mc.lwt_retain = 1;
  mc.reconnect_timeout_ms = 10000;
#endif
  gMqtt = esp_mqtt_client_init(&mc);
  if (!gMqtt) { Serial.println("[MQTT] init failed"); return; }
  esp_mqtt_client_register_event(gMqtt, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqttEvt, nullptr);
  esp_mqtt_client_start(gMqtt);
  Serial.printf("[MQTT] connecting to %s\n", gMqUri.c_str());
}

static void doMqtt(const Obs &o) {
  String j = obsJson(o);
  int id = esp_mqtt_client_publish(gMqtt, gMqState.c_str(), j.c_str(), j.length(), 0, 1);
  upRecord(gUpMq, id, id >= 0);
}

static bool due(uint32_t t, uint16_t s) { return t == 0 || millis() - t >= (uint32_t)s * 1000UL; }

static void uploadTask(void *) {
  uint32_t tWu = 0, tWy = 0, tMq = 0, tHt = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (WiFi.status() != WL_CONNECTED) continue;
    Obs o;
    getObs(o);
    if (!o.valid) continue;
    uint32_t maxAge = (uint32_t)cfg.poll * 3000UL + 10000UL;
    if (millis() - o.ms > maxAge) continue;                 // never upload stale data
    if (cfg.wuEn && cfg.wuId.length() && due(tWu, cfg.wuInt))   { tWu = millis(); doWu(o); }
    if (cfg.wyEn && cfg.wyId.length() && due(tWy, cfg.wyInt))   { tWy = millis(); doWindy(o); }
    if (cfg.htEn && cfg.htUrl.length() && due(tHt, cfg.htInt))  { tHt = millis(); doHttp(o); }
    if (cfg.mqEn && gMqtt && gMqttUp && due(tMq, cfg.mqInt))    { tMq = millis(); doMqtt(o); }
  }
}

// ------------------------------------------------------------------------------------------------
// WiFi state machine: STA with retries -> fallback AP + captive portal -> back to STA
// ------------------------------------------------------------------------------------------------
static volatile NetState gNet = NET_CONNECTING;
static uint16_t  gAttempt = 0;
static uint32_t  gAttemptStart = 0, gLastPortalTry = 0, gLastApClient = 0;
static bool      gPortalForced = false, gStaUpInPortal = false, gNetInit = false;
static DNSServer gDns;
static WebServer server(80);
static uint32_t  gRebootAt = 0;

static void onWiFiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] got IP %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFi] STA disconnected, reason %u\n", info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("[WiFi] client joined the config AP");
      break;
    default:
      break;
  }
}

static void onStaUp() {
  Serial.printf("[WiFi] online: %s, IP %s, RSSI %d dBm\n", cfg.ssid.c_str(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (gNetInit) return;
  gNetInit = true;
  configTzTime(cfg.tz.c_str(), cfg.ntp.c_str());
  if (MDNS.begin(cfg.host.c_str())) MDNS.addService("http", "tcp", 80);
  mqttStart();
}

static void staBegin() {
  gAttempt++;
  gAttemptStart = millis();
  Serial.printf("[WiFi] connecting to '%s' (attempt %u/%u)\n", cfg.ssid.c_str(), gAttempt, cfg.maxAtt);
  WiFi.disconnect(false, false);
  delay(50);
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
}

static void applyStaticIp() {
  if (!cfg.stIp) return;
  IPAddress ip, gw, mk, dn;
  if (ip.fromString(cfg.ip) && gw.fromString(cfg.gw) && mk.fromString(cfg.mask)) {
    if (!dn.fromString(cfg.dns)) dn = gw;
    WiFi.config(ip, gw, mk, dn);
  } else {
    Serial.println("[WiFi] static IP settings invalid, using DHCP");
  }
}

static void startSta() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);             // reconnection is handled here
  applyStaticIp();
  gAttempt = 0;
  gNet = NET_CONNECTING;
  staBegin();
}

static void startPortal(bool forced, const char *why) {
  Serial.printf("[WiFi] opening config AP '%s' (%s)\n", gApSsid.c_str(), why);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(gApSsid.c_str(), cfg.apPass.c_str());
  gDns.setErrorReplyCode(DNSReplyCode::NoError);
  gDns.start(53, "*", WiFi.softAPIP());
  gNet = NET_PORTAL;
  gPortalForced = forced;
  gStaUpInPortal = false;
  gLastPortalTry = gLastApClient = millis();
}

static void stopPortal() {
  gDns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  gNet = NET_ONLINE;
  Serial.println("[WiFi] config AP closed, back to station mode");
}

static void wifiTick() {
  bool up = WiFi.status() == WL_CONNECTED;
  switch (gNet) {
    case NET_CONNECTING:
      if (up) { gNet = NET_ONLINE; onStaUp(); }
      else if (millis() - gAttemptStart > STA_ATTEMPT_MS) {
        if (gAttempt >= cfg.maxAtt) startPortal(false, "connection attempts exhausted");
        else staBegin();
      }
      break;

    case NET_ONLINE:
      if (!up) {
        Serial.println("[WiFi] link lost, reconnecting");
        gAttempt = 0;
        gNet = NET_CONNECTING;
        staBegin();
      }
      break;

    case NET_PORTAL: {
      gDns.processNextRequest();
      uint8_t clients = WiFi.softAPgetStationNum();
      if (clients) gLastApClient = millis();
      if (up) {
        if (!gStaUpInPortal) { gStaUpInPortal = true; onStaUp(); }
        uint32_t idle = gPortalForced ? 300000UL : 30000UL;
        if (!clients && millis() - gLastApClient > idle) stopPortal();
      } else {
        gStaUpInPortal = false;
        // retry the configured network only when nobody uses the AP (STA scan disturbs AP clients)
        if (cfg.ssid.length() && !clients && millis() - gLastPortalTry > (uint32_t)cfg.portRetry * 1000UL) {
          gLastPortalTry = millis();
          Serial.printf("[WiFi] AP mode: retrying '%s'\n", cfg.ssid.c_str());
          WiFi.disconnect(false, false);
          delay(50);
          WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
        }
      }
    } break;
  }
}

// ------------------------------------------------------------------------------------------------
// Button + LED
// ------------------------------------------------------------------------------------------------
static uint32_t gBtnDown = 0, gBtnHeld = 0;

static void buttonTick() {
  bool pressed = digitalRead(BOOT_BTN_PIN) == LOW;
  if (pressed) {
    if (!gBtnDown) gBtnDown = millis();
    gBtnHeld = millis() - gBtnDown;
  } else if (gBtnDown) {
    uint32_t held = millis() - gBtnDown;
    gBtnDown = gBtnHeld = 0;
    if (held >= 10000) factoryReset();
    else if (held >= 3000 && gNet != NET_PORTAL) startPortal(true, "BOOT button");
  }
}

static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t last = 0xFFFFFFFF;
  uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (v == last) return;
  last = v;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(RGB_LED_PIN, r, g, b);
#else
  neopixelWrite(RGB_LED_PIN, r, g, b);
#endif
}

static void ledTick() {
  static uint32_t t = 0;
  if (millis() - t < 100) return;
  t = millis();
  bool blink = (millis() / 400) & 1;
  const uint8_t L = LED_BRIGHTNESS;
  if (gBtnHeld >= 10000)          ledSet(L, 0, 0);
  else if (gBtnHeld >= 3000)      ledSet(0, L, L);
  else if (gNet == NET_PORTAL)    ledSet(0, 0, blink ? L : 0);
  else if (gNet == NET_CONNECTING) ledSet(blink ? L : 0, blink ? L / 2 : 0, 0);
  else if (!gReady)               ledSet(L, 0, L);
  else if (davisFresh())          ledSet(0, L, 0);
  else                            ledSet(L, 0, 0);
}

// ------------------------------------------------------------------------------------------------
// Web UI
// ------------------------------------------------------------------------------------------------
// Plain string literal (no raw string): the Arduino prototype scanner does not parse raw strings
static const char CSS[] PROGMEM =
  "body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:#f3f4f6;color:#1f2328}"
  "header{background:#1f3b57;color:#fff;padding:10px 14px}header b{font-size:1.1em}"
  "nav a{color:#cfe3ff;margin-right:12px;text-decoration:none}"
  "main{padding:8px;max-width:760px;margin:auto}"
  "section,fieldset{background:#fff;border:1px solid #d0d7de;border-radius:8px;margin:10px 0;padding:10px 12px}"
  "h2,legend{font-size:1em;font-weight:600;margin:2px 0 6px}"
  "label{display:block;margin:8px 0 2px;font-size:.9em;color:#444}"
  "input,select{width:100%;padding:7px;box-sizing:border-box;border:1px solid #b6bec8;border-radius:5px;font-size:1em}"
  "input[type=checkbox]{width:auto;margin-right:6px}"
  "table{width:100%;border-collapse:collapse}td{padding:4px 2px;border-bottom:1px solid #eee;font-size:.95em}"
  "td:first-child{color:#555;width:48%}"
  "button,.btn{background:#1f3b57;color:#fff;border:0;border-radius:6px;padding:9px 14px;margin:4px 2px;"
  "font-size:1em;text-decoration:none;display:inline-block;cursor:pointer}"
  ".red{background:#b42318}.ok{color:#1a7f37;font-weight:600}.bad{color:#b42318;font-weight:600}"
  ".row{display:flex;gap:6px;align-items:flex-end}.row>*{flex:1}.small{font-size:.85em;color:#666}";

static bool viaAp() {
  return gNet == NET_PORTAL && server.client().localIP() == WiFi.softAPIP();
}

static bool auth() {
  if (viaAp()) return true;                         // AP is WPA2 protected; captive sheets can't do Basic auth
  if (server.authenticate("admin", cfg.admPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "DavisVP2");
  return false;
}

// Overloads instead of default arguments (safer with the Arduino prototype generator)
static void pageBegin(String &h, const char *title, int refresh) {
  h += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  if (refresh > 0) { h += F("<meta http-equiv=\"refresh\" content=\""); h += refresh; h += F("\">"); }
  h += F("<title>"); h += title; h += F(" - Davis VP2</title><style>"); h += FPSTR(CSS); h += F("</style></head><body>");
  h += F("<header><b>Davis VP2 gateway</b> <span class=small style=\"color:#cfe3ff\">"); h += htmlEsc(cfg.host);
  h += F("</span><nav><a href=\"/\">Status</a><a href=\"/config\">Settings</a><a href=\"/update\">Firmware</a><a href=\"/api/data\">JSON</a></nav></header><main>");
}
static void pageBegin(String &h, const char *title) { pageBegin(h, title, 0); }

static void pageEnd(String &h) {
  h += F("<p class=small>FW " FW_VERSION "</p></main></body></html>");
}

static void sendHtml(const String &h) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", h);
}

static void row(String &h, const char *k, const String &v, int cls) {
  h += F("<tr><td>"); h += k; h += F("</td><td");
  if (cls == 1) h += F(" class=ok");
  else if (cls == 2) h += F(" class=bad");
  h += '>'; h += v; h += F("</td></tr>");
}
static void row(String &h, const char *k, const String &v) { row(h, k, v, 0); }

static String fv(float v, int dec, const char *unit) {
  if (isnan(v)) return "n/a";
  String s(v, dec);
  s += ' ';
  s += unit;
  return s;
}

static String ago(uint32_t ms) {
  if (!ms) return "never";
  return String((millis() - ms) / 1000) + " s ago";
}

static String upLine(bool en, const UpStat &s) {
  if (!en) return String("disabled");
  String r = String(s.ok) + " ok / " + String(s.fail) + " fail, last " + ago(s.lastMs);
  if (s.lastMs) { r += " ("; r += s.lastCode; r += ')'; }
  return r;
}

static void handleRoot() {
  if (!auth()) return;
  Obs o;
  getObs(o);
  String h;
  h.reserve(7000);
  pageBegin(h, "Status", 10);

  h += F("<section><h2>Current conditions</h2><table>");
  row(h, "Outside temperature", fv(o.tOut, 1, "&deg;C"));
  row(h, "Outside humidity",    fv(o.hOut, 0, "%"));
  row(h, "Dew point",           fv(o.dew, 1, "&deg;C"));
  row(h, "Wind",                fv(o.wind, 1, "m/s") + " / " + fv(o.windDir, 0, "&deg;"));
  row(h, "Wind avg 2 min / 10 min", fv(o.windAvg2, 1, "m/s") + " / " + fv(o.windAvg10, 1, "m/s"));
  row(h, "Gust 10 min",         fv(o.gust10, 1, "m/s") + " / " + fv(o.gustDir10, 0, "&deg;"));
  row(h, "Barometer (sea level)", fv(o.baro, 1, "hPa"));
  row(h, "Station pressure",    fv(o.pressAbs, 1, "hPa"));
  row(h, "Rain rate",           fv(o.rainRate, 1, "mm/h"));
  row(h, "Rain today / 1 h / 24 h", fv(o.rainDay, 1, "mm") + " / " + fv(o.rain1h, 1, "mm") + " / " + fv(o.rain24, 1, "mm"));
  row(h, "Rain storm / month / year", fv(o.rainStorm, 1, "mm") + " / " + fv(o.rainMonth, 1, "mm") + " / " + fv(o.rainYear, 1, "mm"));
  row(h, "Wind chill",          fv(o.windChill, 1, "&deg;C"));
  row(h, "Heat index",          fv(o.heatIdx, 1, "&deg;C"));
  if (cfg.sSolar) {
    row(h, "Solar radiation",   fv(o.solar, 0, "W/m&sup2;"));
    row(h, "THSW index",        fv(o.thsw, 0, "&deg;C"));
    row(h, "ET today",          fv(o.etDay, 2, "mm"));
  }
  if (cfg.sUv) row(h, "UV index", fv(o.uv, 1, ""));
  row(h, "Inside",              fv(o.tIn, 1, "&deg;C") + " / " + fv(o.hIn, 0, "%"));
  row(h, "Data age",            o.valid ? String(String((millis() - o.ms) / 1000) + " s") : String("no data"));
  h += F("</table></section>");

  if (cfg.sExtra || cfg.sSoil) {
    h += F("<section><h2>Optional stations</h2><table>");
    bool any = false;
    char k[24];
    for (int i = 0; i < 7; i++) {
      if (isnan(o.xT[i]) && isnan(o.xH[i])) continue;
      snprintf(k, sizeof(k), "Extra station %d", i + 1);
      row(h, k, fv(o.xT[i], 1, "&deg;C") + " / " + fv(o.xH[i], 0, "%"));
      any = true;
    }
    for (int i = 0; i < 4; i++) {
      if (isnan(o.soilT[i]) && isnan(o.soilM[i])) continue;
      snprintf(k, sizeof(k), "Soil %d", i + 1);
      row(h, k, fv(o.soilT[i], 1, "&deg;C") + " / " + fv(o.soilM[i], 0, "cb"));
      any = true;
    }
    for (int i = 0; i < 4; i++) {
      if (isnan(o.leafT[i]) && (i >= 2 || isnan(o.leafW[i]))) continue;
      snprintf(k, sizeof(k), "Leaf %d", i + 1);
      String v = fv(o.leafT[i], 1, "&deg;C");
      if (i < 2) { v += " / wetness "; v += isnan(o.leafW[i]) ? String("n/a") : String((int)o.leafW[i]); }
      row(h, k, v);
      any = true;
    }
    if (!any) row(h, "No data yet", String("values arrive with the LOOP packet, every ") + String(cfg.l1Every * cfg.poll) + " s");
    h += F("</table></section>");
  }

  h += F("<section><h2>Console / logger</h2><table>");
  String st = davisState();
  row(h, "State", st, st == "OK" ? 1 : 2);
  char vp[16];
  snprintf(vp, sizeof(vp), "%04X:%04X", gVid, gPid);
  row(h, "USB device", gReady ? String(vp) : String("none"));
  row(h, "Packets LOOP2 / LOOP", String(dstat.ok2) + " / " + String(dstat.ok1));
  row(h, "CRC / malformed / timeouts", String(dstat.crcErr) + " / " + String(dstat.badPkt) + " / " + String(dstat.timeouts));
  row(h, "Wake failures / NAK", String(dstat.wakeFail) + " / " + String(dstat.nak));
  row(h, "Mode", dstat.lps ? "LPS (LOOP2) + LOOP" : "LOOP only");
  row(h, "Rain collector", String(dstat.rainSrc));
  {
    static const char *const wcl[] = {"console", "calculated (current wind)", "calculated (2 min average)", "calculated (10 min average)"};
    String src = "wind chill: ";
    src += wcl[cfg.wcMode];
    src += ", heat index: ";  src += cfg.hiMode ? "calculated" : "console";
    src += ", dew point: ";   src += cfg.dpMode ? "calculated" : "console";
    row(h, "Derived values", src);
  }
  row(h, "Console battery", fv(o.consBattV, 2, "V"));
  String txb("n/a");
  if (!isnan(o.txBatt)) { txb = String((int)o.txBatt); txb += (int)o.txBatt ? " (low)" : " (ok)"; }
  row(h, "Transmitter battery flags", txb);
  String cs("disabled");
  if (cfg.tSync) { cs = String(dstat.timeSyncs); cs += " done, last "; cs += ago(dstat.lastTimeSyncMs); }
  row(h, "Clock sync", cs);
  row(h, "Last error", htmlEsc(String(dstat.lastErr)));
  h += F("</table></section>");

  h += F("<section><h2>Network</h2><table>");
  const char *mode = gNet == NET_PORTAL ? "Config AP" : (gNet == NET_ONLINE ? "Station" : "Connecting");
  row(h, "Mode", mode, gNet == NET_ONLINE ? 1 : 2);
  row(h, "SSID", htmlEsc(cfg.ssid));
  if (WiFi.status() == WL_CONNECTED) {
    row(h, "IP", WiFi.localIP().toString());
    row(h, "RSSI", String(WiFi.RSSI()) + " dBm");
  } else if (gNet == NET_CONNECTING) {
    row(h, "Attempt", String(gAttempt) + " / " + String(cfg.maxAtt));
  }
  if (gNet == NET_PORTAL) row(h, "AP", htmlEsc(gApSsid) + " (192.168.4.1)");
  row(h, "Hostname", htmlEsc(cfg.host) + ".local");
  h += F("</table></section>");

  h += F("<section><h2>Uploads</h2><table>");
  row(h, "Weather Underground", upLine(cfg.wuEn, gUpWu));
  row(h, "Windy",               upLine(cfg.wyEn, gUpWy));
  String mq("disabled");
  if (cfg.mqEn) { mq = gMqttUp ? "connected, " : "not connected, "; mq += upLine(true, gUpMq); }
  row(h, "MQTT",                mq);
  row(h, "HTTP POST",           upLine(cfg.htEn, gUpHt));
  h += F("</table></section>");

  h += F("<section><h2>System</h2><table>");
  row(h, "Uptime", String(millis() / 1000) + " s");
  row(h, "Free heap / PSRAM", String(ESP.getFreeHeap() / 1024) + " kB / " + String(ESP.getFreePsram() / 1024) + " kB");
  h += F("</table><form method=POST action=\"/reboot\" style=\"display:inline\"><button>Reboot</button></form></section>");
  pageEnd(h);
  sendHtml(h);
}

// --- settings form helpers ---
static void fIn(String &h, const char *label, const char *name, const String &val, const char *type, const char *attrs) {
  h += F("<label>"); h += label; h += F("</label><input type=\""); h += type; h += F("\" name=\""); h += name;
  h += F("\" value=\""); h += htmlEsc(val); h += F("\" "); h += attrs; h += '>';
}
static void fIn(String &h, const char *label, const char *name, const String &val) { fIn(h, label, name, val, "text", ""); }
static void fNum(String &h, const char *label, const char *name, uint32_t val, uint32_t lo, uint32_t hi) {
  char a[48];
  snprintf(a, sizeof(a), "min=\"%lu\" max=\"%lu\"", (unsigned long)lo, (unsigned long)hi);
  fIn(h, label, name, String(val), "number", a);
}
static void fPass(String &h, const char *label, const char *name, const String &cur) {
  fIn(h, label, name, cur.length() ? String(KEEP_SENTINEL) : String(""), "password", "autocomplete=\"new-password\"");
}
static void fChk(String &h, const char *label, const char *name, bool v) {
  h += F("<label><input type=\"checkbox\" name=\""); h += name; h += F("\" value=\"1\"");
  if (v) h += F(" checked");
  h += F(">"); h += label; h += F("</label>");
}
static void fSel(String &h, const char *label, const char *name, const String &cur,
                 const char *const *vals, const char *const *labels, int n) {
  h += F("<label>"); h += label; h += F("</label><select name=\""); h += name; h += F("\">");
  for (int i = 0; i < n; i++) {
    h += F("<option value=\""); h += vals[i]; h += '"';
    if (cur == vals[i]) h += F(" selected");
    h += '>'; h += labels[i]; h += F("</option>");
  }
  h += F("</select>");
}

static void handleConfig() {
  if (!auth()) return;
  String h;
  h.reserve(12000);
  pageBegin(h, "Settings");
  h += F("<form method=POST action=\"/save\">");

  h += F("<fieldset><legend>WiFi</legend>");
  h += F("<div class=row><div>");
  fIn(h, "SSID", "ssid", cfg.ssid, "text", "list=\"ssids\" autocapitalize=\"none\"");
  h += F("</div><div style=\"flex:0 0 auto\"><button type=button id=sb onclick=\"scan()\">Scan</button></div></div><datalist id=ssids></datalist>");
  fPass(h, "Password", "pass", cfg.pass);
  fIn(h, "Hostname (mDNS)", "host", cfg.host);
  fChk(h, "Static IP", "stip", cfg.stIp);
  h += F("<div class=row><div>"); fIn(h, "IP", "ip", cfg.ip); h += F("</div><div>"); fIn(h, "Gateway", "gw", cfg.gw); h += F("</div></div>");
  h += F("<div class=row><div>"); fIn(h, "Netmask", "mask", cfg.mask); h += F("</div><div>"); fIn(h, "DNS", "dns", cfg.dns); h += F("</div></div>");
  fNum(h, "Failed attempts before opening the config AP (15 s each)", "maxatt", cfg.maxAtt, 1, 100);
  fNum(h, "While in AP mode, retry the network every (s)", "pretry", cfg.portRetry, 15, 3600);
  fPass(h, "Config AP password (min 8 chars)", "appass", cfg.apPass);
  fPass(h, "Web admin password (user: admin)", "admpass", cfg.admPass);
  h += F("</fieldset>");

  h += F("<fieldset><legend>Davis console</legend>");
  {
    static const char *const bv[] = {"19200", "14400", "9600", "4800", "2400", "1200"};
    fSel(h, "Console baud rate", "baud", String(cfg.baud), bv, bv, 6);
    static const char *const rv[] = {"auto", "0.2", "0.1", "0.254"};
    static const char *const rl[] = {"Auto (read from console)", "0.2 mm", "0.1 mm", "0.01 in"};
    fSel(h, "Rain collector", "rain", cfg.rain, rv, rl, 4);
  }
  fNum(h, "Poll interval (s)", "poll", cfg.poll, 3, 3600);
  fNum(h, "Read LOOP packet (batteries, month/year rain, optional stations) every N polls", "l1every", cfg.l1Every, 1, 1000);
  fChk(h, "Set console clock from NTP once a day", "tsync", cfg.tSync);
  fIn(h, "Time zone (POSIX TZ)", "tz", cfg.tz);
  fIn(h, "NTP server", "ntp", cfg.ntp);
  h += F("</fieldset>");

  h += F("<fieldset><legend>Derived values</legend><p class=small>The console reports these in whole &deg;F "
         "(steps of about 0.6 &deg;C). Calculated values use the 0.1 &deg;F temperature. When the console "
         "doesn't report a value, it is always calculated.</p>");
  {
    static const char *const wv[] = {"0", "1", "2", "3"};
    static const char *const wl[] = {"From the console", "Calculated, current wind",
                                     "Calculated, 2 minute average wind", "Calculated, 10 minute average wind"};
    fSel(h, "Wind chill", "wcmode", String(cfg.wcMode), wv, wl, 4);
    static const char *const hv[] = {"0", "1"};
    static const char *const hl[] = {"From the console", "Calculated (NWS formula)"};
    fSel(h, "Heat index", "himode", String(cfg.hiMode), hv, hl, 2);
    static const char *const dl[] = {"From the console", "Calculated (Magnus formula)"};
    fSel(h, "Dew point", "dpmode", String(cfg.dpMode), hv, dl, 2);
  }
  h += F("</fieldset>");

  h += F("<fieldset><legend>Optional sensors</legend><p class=small>Untick what your station doesn't have. "
         "Unticked sensors are never shown or uploaded. Extra and soil/leaf stations are read with the LOOP "
         "packet, so they update at that rate.</p>");
  fChk(h, "Solar radiation sensor (also needed for THSW index and evapotranspiration)", "ssolar", cfg.sSolar);
  fChk(h, "UV sensor", "suv", cfg.sUv);
  fChk(h, "Extra temperature / humidity stations (up to 7)", "sextra", cfg.sExtra);
  fChk(h, "Soil / leaf station (4 soil temperatures and moistures, leaf temperatures, 2 leaf wetness)", "ssoil", cfg.sSoil);
  h += F("</fieldset>");

  h += F("<fieldset><legend>Weather Underground protocol</legend>");
  fChk(h, "Enabled", "wuen", cfg.wuEn);
  fIn(h, "Upload URL (WU default; PWSweather etc. accept the same parameters)", "wuurl", cfg.wuUrl);
  fIn(h, "Station ID", "wuid", cfg.wuId);
  fPass(h, "Station key / password", "wukey", cfg.wuKey);
  fNum(h, "Interval (s)", "wuint", cfg.wuInt, 3, 3600);
  h += F("</fieldset>");

  h += F("<fieldset><legend>Windy (API v2)</legend>");
  fChk(h, "Enabled", "wyen", cfg.wyEn);
  fIn(h, "Station ID", "wyid", cfg.wyId);
  fPass(h, "Station password", "wypass", cfg.wyPass);
  fNum(h, "Interval (s, min 300)", "wyint", cfg.wyInt, 300, 3600);
  h += F("</fieldset>");

  h += F("<fieldset><legend>MQTT</legend>");
  fChk(h, "Enabled", "mqen", cfg.mqEn);
  h += F("<div class=row><div>"); fIn(h, "Broker host", "mqhost", cfg.mqHost); h += F("</div><div style=\"flex:0 0 30%\">");
  fNum(h, "Port", "mqport", cfg.mqPort, 1, 65535); h += F("</div></div>");
  fIn(h, "User", "mquser", cfg.mqUser);
  fPass(h, "Password", "mqpass", cfg.mqPass);
  fIn(h, "Base topic (publishes &lt;base&gt;/state and &lt;base&gt;/status)", "mqtopic", cfg.mqTopic);
  fNum(h, "Interval (s)", "mqint", cfg.mqInt, 2, 3600);
  h += F("</fieldset>");

  h += F("<fieldset><legend>HTTP POST (JSON)</legend>");
  fChk(h, "Enabled", "hten", cfg.htEn);
  fIn(h, "URL", "hturl", cfg.htUrl);
  fPass(h, "Bearer token (optional)", "httok", cfg.htTok);
  fNum(h, "Interval (s)", "htint", cfg.htInt, 2, 3600);
  h += F("</fieldset>");

  h += F("<button>Save and reboot</button></form>");
  h += F("<form method=POST action=\"/reset\" onsubmit=\"return confirm('Erase all settings?')\"><button class=red>Factory reset</button></form>");
  h += F("<script>function scan(){var b=document.getElementById('sb');b.disabled=true;b.textContent='...';var n=0;"
         "(function p(){fetch('/scan').then(function(r){return r.json()}).then(function(j){"
         "if(j.busy&&n++<20){setTimeout(p,1000);return}"
         "var d=document.getElementById('ssids'),s={};d.innerHTML='';(j.list||[]).sort(function(a,c){return c.r-a.r}).forEach(function(x){"
         "if(!x.s||s[x.s])return;s[x.s]=1;var o=document.createElement('option');o.value=x.s;o.label=x.r+' dBm';d.appendChild(o)});"
         "b.disabled=false;b.textContent='Scan'}).catch(function(){b.disabled=false;b.textContent='Scan'})})()}</script>");
  pageEnd(h);
  sendHtml(h);
}

static void handleSave() {
  if (!auth()) return;
  auto S = [](const char *n, String &d) { if (server.hasArg(n)) { d = server.arg(n); d.trim(); } };
  auto P = [](const char *n, String &d) { if (server.hasArg(n) && server.arg(n) != KEEP_SENTINEL) d = server.arg(n); };
  auto U = [](const char *n, uint16_t &d, long lo, long hi) {
    if (server.hasArg(n)) { long v = server.arg(n).toInt(); if (v >= lo && v <= hi) d = (uint16_t)v; }
  };
  auto B = [](const char *n) { return server.hasArg(n); };

  // Work on a copy: the live cfg is read by other tasks until the reboot
  Config c = cfg;
  S("ssid", c.ssid);   P("pass", c.pass);   S("host", c.host);
  c.stIp = B("stip");  S("ip", c.ip);       S("gw", c.gw);   S("mask", c.mask);  S("dns", c.dns);
  U("maxatt", c.maxAtt, 1, 100);  U("pretry", c.portRetry, 15, 3600);
  { String a = c.apPass;  P("appass", a);  if (a.length() >= 8 && a.length() <= 63) c.apPass = a; }
  { String a = c.admPass; P("admpass", a); if (a.length() >= 4) c.admPass = a; }
  if (server.hasArg("baud")) c.baud = (uint32_t)server.arg("baud").toInt();
  S("rain", c.rain);
  if (server.hasArg("wcmode")) c.wcMode = (uint8_t)server.arg("wcmode").toInt();
  if (server.hasArg("himode")) c.hiMode = (uint8_t)server.arg("himode").toInt();
  if (server.hasArg("dpmode")) c.dpMode = (uint8_t)server.arg("dpmode").toInt();
  c.sSolar = B("ssolar"); c.sUv = B("suv"); c.sExtra = B("sextra"); c.sSoil = B("ssoil");
  U("poll", c.poll, 3, 3600);     U("l1every", c.l1Every, 1, 1000);
  c.tSync = B("tsync"); S("tz", c.tz);      S("ntp", c.ntp);
  c.wuEn = B("wuen");   S("wuurl", c.wuUrl); S("wuid", c.wuId);   P("wukey", c.wuKey);   U("wuint", c.wuInt, 3, 3600);
  c.wyEn = B("wyen");   S("wyid", c.wyId);   P("wypass", c.wyPass); U("wyint", c.wyInt, 300, 3600);
  c.mqEn = B("mqen");   S("mqhost", c.mqHost); U("mqport", c.mqPort, 1, 65535); S("mquser", c.mqUser);
  P("mqpass", c.mqPass); S("mqtopic", c.mqTopic); U("mqint", c.mqInt, 2, 3600);
  c.htEn = B("hten");   S("hturl", c.htUrl); P("httok", c.htTok);   U("htint", c.htInt, 2, 3600);

  saveConfig(c);
  Serial.println("[CFG] saved, rebooting");
  String h;
  pageBegin(h, "Saved");
  h += F("<section><h2>Saved</h2><p>Rebooting. If the WiFi settings are correct the device joins <b>");
  h += htmlEsc(c.ssid);
  h += F("</b>: open <b>http://"); h += htmlEsc(c.host); h += F(".local/</b> or its IP. ");
  h += F("If it cannot connect, the config AP <b>"); h += htmlEsc(gApSsid); h += F("</b> comes back after the retries.</p></section>");
  pageEnd(h);
  sendHtml(h);
  gRebootAt = millis() + 2000;
}

static void handleScan() {
  if (!auth()) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); n = WIFI_SCAN_RUNNING; }
  if (n == WIFI_SCAN_RUNNING) { server.send(200, "application/json", "{\"busy\":true}"); return; }
  String j = "{\"busy\":false,\"list\":[";
  for (int i = 0; i < n; i++) {
    if (i) j += ',';
    j += "{\"s\":\""; j += jsonEsc(WiFi.SSID(i)); j += "\",\"r\":"; j += WiFi.RSSI(i); j += '}';
  }
  j += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

static void handleUpdatePage() {
  if (!auth()) return;
  String h;
  pageBegin(h, "Firmware");
  h += F("<section><h2>Firmware update</h2><p class=small>Arduino IDE: Sketch &gt; Export Compiled Binary, then upload "
         "<code>DavisVP2_Gateway.ino.bin</code> (not the merged/bootloader image).</p>"
         "<form method=POST action=\"/update\" enctype=\"multipart/form-data\">"
         "<input type=file name=fw accept=\".bin\"><button>Upload</button></form></section>");
  pageEnd(h);
  sendHtml(h);
}

static void handleUpdateDone() {
  if (!auth()) return;
  bool ok = !Update.hasError();
  String h;
  pageBegin(h, "Firmware");
  h += ok ? F("<section><h2 class=ok>Update OK</h2><p>Rebooting.</p></section>")
          : F("<section><h2 class=bad>Update failed</h2><p>See the serial log.</p></section>");
  pageEnd(h);
  sendHtml(h);
  if (ok) gRebootAt = millis() + 1500;
}

static void handleUpdateUpload() {
  if (!viaAp() && !server.authenticate("admin", cfg.admPass.c_str())) return;
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] receiving %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("[OTA] done, %u bytes\n", (unsigned)up.totalSize);
    else Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("[OTA] aborted");
  }
}

static void setupWeb() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/api/data", HTTP_GET, []() {
    Obs o;
    getObs(o);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", obsJson(o));
  });
  server.on("/api/status", HTTP_GET, []() {
    if (!auth()) return;
    String j = "{\"fw\":\"" FW_VERSION "\",\"uptime_s\":"; j += millis() / 1000;
    j += ",\"net\":\""; j += gNet == NET_PORTAL ? "portal" : (gNet == NET_ONLINE ? "online" : "connecting");
    j += "\",\"rssi\":"; j += WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    j += ",\"usb\":"; j += gReady ? "true" : "false";
    j += ",\"davis\":\""; j += jsonEsc(davisState());
    j += "\",\"loop2\":"; j += dstat.ok2; j += ",\"loop1\":"; j += dstat.ok1;
    j += ",\"crc_err\":"; j += dstat.crcErr; j += ",\"timeouts\":"; j += dstat.timeouts;
    j += ",\"wu_ok\":"; j += gUpWu.ok; j += ",\"wu_fail\":"; j += gUpWu.fail;
    j += ",\"windy_ok\":"; j += gUpWy.ok; j += ",\"windy_fail\":"; j += gUpWy.fail;
    j += ",\"mqtt\":"; j += gMqttUp ? "true" : "false";
    j += ",\"http_ok\":"; j += gUpHt.ok; j += ",\"http_fail\":"; j += gUpHt.fail;
    j += ",\"heap\":"; j += ESP.getFreeHeap(); j += '}';
    server.send(200, "application/json", j);
  });
  server.on("/reboot", HTTP_POST, []() {
    if (!auth()) return;
    String h; pageBegin(h, "Reboot"); h += F("<section><p>Rebooting...</p></section>"); pageEnd(h); sendHtml(h);
    gRebootAt = millis() + 1000;
  });
  server.on("/reset", HTTP_POST, []() {
    if (!auth()) return;
    String h; pageBegin(h, "Reset"); h += F("<section><p>Settings erased. Rebooting into config AP mode.</p></section>"); pageEnd(h); sendHtml(h);
    delay(500);
    factoryReset();
  });
  server.onNotFound([]() {
    if (viaAp()) {                                   // captive portal: send every probe to the settings page
      server.sendHeader("Location", "http://192.168.4.1/config", true);
      server.send(302, "text/plain", "");
      return;
    }
    server.send(404, "text/plain", "Not found");
  });
  server.begin();
}

// ------------------------------------------------------------------------------------------------
// setup / loop
// ------------------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== Davis VP2 gateway %s ===\n", FW_VERSION);
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  ledSet(0, 0, LED_BRIGHTNESS);

  uint64_t mac = ESP.getEfuseMac();
  char sfx[8];
  snprintf(sfx, sizeof(sfx), "%02X%02X%02X", (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  gMacSuffix = sfx;
  gApSsid = String("DavisVP2-") + gMacSuffix;

  loadConfig();
  memset(&dstat, 0, sizeof(dstat));
  dstat.lps = true;
  strlcpy(dstat.rainSrc, "n/a", sizeof(dstat.rainSrc));
  applyRainSetting();

  gObsMtx = xSemaphoreCreateMutex();
  gOutSem = xSemaphoreCreateBinary();
  gOutMtx = xSemaphoreCreateMutex();
  gRx     = xStreamBufferCreate(4096, 1);
  obsReset(gObs);

  usb_host_config_t hc = {};
  hc.skip_phy_setup = false;
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  esp_err_t e = usb_host_install(&hc);
  if (e == ESP_OK) {
    xTaskCreatePinnedToCore(usbLibTask, "usb_lib", 4096, nullptr, 10, nullptr, 0);
    xTaskCreatePinnedToCore(usbClientTask, "usb_cp210x", 6144, nullptr, 9, nullptr, 0);
    Serial.println("[USB] host started, waiting for the Davis logger");
  } else {
    Serial.printf("[USB] host install failed: %s\n", esp_err_to_name(e));
  }
  xTaskCreatePinnedToCore(davisTask, "davis", 6144, nullptr, 5, nullptr, 1);

  WiFi.persistent(false);                         // credentials live only in our NVS namespace
  WiFi.onEvent(onWiFiEvent);
  WiFi.setHostname(cfg.host.c_str());
  if (cfg.ssid.isEmpty()) startPortal(false, "no WiFi configured");
  else startSta();
  setupWeb();

  xTaskCreatePinnedToCore(uploadTask, "upload", 16384, nullptr, 3, nullptr, 0);
}

void loop() {
  wifiTick();
  server.handleClient();
  buttonTick();
  ledTick();
  if (gRebootAt && (int32_t)(millis() - gRebootAt) >= 0) ESP.restart();
  delay(2);
}
