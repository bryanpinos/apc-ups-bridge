// APC UPS -> MQTT bridge  |  v0.1 "diagnostics + OTA"
//
// Adafruit ESP32-S3 Feather (8MB, no PSRAM) + USB Host FeatherWing (MAX3421E)
//   MAX3421E: SPI, CS = pin 10, IRQ = pin 9   (FeatherWing defaults)
//   Native USB-C stays free for serial console -- host runs on rhport 1.
//
// This build does NOT decode UPS data yet. It brings up WiFi + OTA + MQTT,
// publishes diagnostics, and dumps the HID report descriptor of whatever
// USB device is attached so the decode layer can be written against it.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_MAX1704X.h>
#include "Adafruit_TinyUSB.h"
#include "SPI.h"
#include "secrets.h"
#include <esp_system.h>

// ----------------------------------------------------------------- config
static const char *FW_VERSION = "1.9.1";
// ===================== PER-UNIT IDENTITY =====================
// Define these in secrets.h (gitignored) to build a second bridge. Defaults below
// are placeholders so the project builds out of the box.
//   UNIT_ID   MQTT client id, topic base, HA unique_id prefix. Underscores ok.
//   UNIT_HOST Network hostname. RFC-1123: letters/digits/hyphens ONLY.
//   UNIT_NAME HA device name. NOTE: HA derives entity_ids from THIS, not UNIT_ID.
#ifndef UNIT_ID
#define UNIT_ID    "ups_bridge"
#endif
#ifndef UNIT_HOST
#define UNIT_HOST  "ups-bridge"
#endif
#ifndef UNIT_NAME
#define UNIT_NAME  "UPS Bridge"
#endif
// =============================================================

static const char *DEV_ID   = UNIT_ID;
static const char *DEV_NAME = UNIT_NAME;
static const char *NET_HOST = UNIT_HOST;

#define MAX3421_CS   10
#define MAX3421_INT   9

static const uint32_t DIAG_INTERVAL_MS   = 15000;
// UPS readings are published on THIS timer, never only from the USB completion
// callback: a single timed-out GET_REPORT used to phase-shift the poll sweep so
// the wrap was never seen again and the ups topic went silent for good (both
// units, 9 days, every health flag still true). See v1.7.1 notes.
static const uint32_t UPS_PUBLISH_MS     = 10000;
static const uint32_t MQTT_RETRY_MS      = 5000;
static const size_t   MQTT_BUFFER_BYTES  = 8192;

// ----------------------------------------------------------------- state
Adafruit_USBH_Host USBHost(&SPI, MAX3421_CS, MAX3421_INT);
Adafruit_MAX17048  fuelGauge;
bool fuelGaugeOk = false;

WiFiClient   net;
PubSubClient mqtt(net);
static WebServer http(80);

static char tBase[48], tStatus[80], tDiag[80], tDesc[80], tLog[80], tCmd[80], tStage[80], tBoot[80], tUps[80];

static volatile bool usbAttached = false;
static uint16_t usbVid = 0, usbPid = 0;
static uint8_t  usbDaddr = 0;

static bool     hidMounted = false;
static uint8_t  hidDaddr = 0, hidIdx = 0;
static uint16_t hidDescLen = 0;
static uint8_t  hidDesc[2048];
static bool     hidDescWanted = false;   // TinyUSB skipped it (>256B enum buf)
static bool     hidDescInFlight = false;
static bool     hidDescDirty = false;
// Set from USBHost.begin()'s return value. hcd_init() reads the MAX3421E's
// REVISION register and TU_ASSERTs it is 0x01/0x12/0x13, so a false return means
// the chip did not answer: no wing, or a wing that is not seated. This is the
// ONLY signal that distinguishes that from "wing fine, nothing plugged in" --
// usb_attached and hid_mounted both describe the DOWNSTREAM device and read
// false either way.
static bool     wingPresent = false;
// MAX3421E pre-flight results; see probeMax3421() below. osc_ms = -1 means the
// crystal never started, which is the signature of a bad joint on U1.
static uint8_t  m3421Rev   = 0;
static int32_t  m3421OscMs = -1;

struct UpsState {
  bool valid = false;
  // status flags
  bool charging=false, discharging=false, acPresent=false, batteryPresent=false;
  bool belowCapLimit=false, shutdownImminent=false, timeLimitExpired=false;
  bool commLost=false, needReplacement=false, overload=false, voltNotRegulated=false;
  int  capacity=-1;        // %
  long runtime=-1;         // seconds
  int  load=-1;            // %
  int  test=-1;
  long vin=-1;             // raw
  long vbatt=-1;           // raw
  int  alarm=-1;
  int  sens=-1;             // APCSensitivity (report 53): 0 low, 1 medium, 2 high
} ups;

static uint32_t pollTimeouts = 0;
static uint32_t submitFails  = 0;   // tuh_hid_get_report() refused (control EP busy)
static uint32_t ep0Aborts    = 0;   // stuck control transfers we aborted
static uint32_t lastApplied  = 0;   // millis() of the last report applied, 0 = never
static uint32_t lastHeal     = 0;
// v1.7.3 USB recovery ladder. ESP.restart() does NOT reset the MAX3421E, so a
// wedged host chip came back from a self-restart blind (usb_attached false for
// three days until a physical power cycle). Recovery now goes through the chip
// itself: bus reset first, then a full host deinit/init (CHIPRES), and only
// then, much later, a restart as the last resort.
static uint32_t busResets    = 0;
static uint32_t hostReinits  = 0;
static uint32_t usbLostSince = 0;   // millis() when the HID device went missing, 0 = mounted
static uint32_t lastUsbHeal  = 0;
static uint8_t  usbHealStep  = 0;
static bool     everMounted  = false;
static const uint8_t USB_RHPORT = 1;   // matches USBHost.begin(1)
// loop() section timing -- worst case since the last diag publish
static uint32_t tmUsb = 0, tmOta = 0, tmMqtt = 0, tmPoll = 0, tmLoop = 0, tmHttp = 0;
static uint8_t  setBuf[8];
static bool     rawDump = false;
static bool upsTest(uint8_t mode);   // defined with the poll machine
static bool upsSetSens(uint8_t v);

static uint32_t lastDiag = 0, lastMqttTry = 0;
static uint32_t lastUpsPub = 0;      // 0 = never published since boot
static bool     upsDirty   = false;  // a report was applied since the last publish
static uint32_t bootMs = 0;

// ----------------------------------------------------------------- helpers
static void logf(const char *fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
  if (mqtt.connected()) mqtt.publish(tLog, buf);
}

static void publishDescriptor() {
  if (!hidDescLen || !mqtt.connected()) return;
  // hex-encode; 1024 bytes max -> 2048 chars, inside the 4k buffer
  static char hex[4200];
  size_t n = hidDescLen > sizeof(hidDesc) ? sizeof(hidDesc) : hidDescLen;
  for (size_t i = 0; i < n; i++) sprintf(hex + i * 2, "%02x", hidDesc[i]);
  hex[n * 2] = 0;
  mqtt.publish(tDesc, hex, true);
  logf("published HID report descriptor: %u bytes", (unsigned)hidDescLen);
  hidDescDirty = false;
}


// ----------------------------------------------------------------- wifi diagnostics
static const char *wifiStatusStr(int st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (auth?)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
  }
}

static const char *authStr(int a) {
  switch (a) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3_PSK";
    default:                        return "other";
  }
}

// Does NOT print the SSID itself -- only whether the configured one is visible.
// The ESP32-S3 radio is 2.4 GHz only, so "NOT FOUND" usually means the SSID is
// 5 GHz-only, hidden, or misspelled.
static void wifiScanReport() {
  int n = WiFi.scanNetworks(false, true);
  Serial.printf("[wifi] scan: %d networks visible on 2.4GHz\n", n);
  bool found = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == String(WIFI_SSID)) {
      found = true;
      Serial.printf("[wifi] configured SSID FOUND  ch=%d rssi=%d auth=%s\n",
                    WiFi.channel(i), (int)WiFi.RSSI(i),
                    authStr(WiFi.encryptionType(i)));
      break;
    }
  }
  if (!found)
    Serial.println("[wifi] configured SSID NOT FOUND on 2.4GHz "
                   "(5GHz-only? hidden? typo?)");
  WiFi.scanDelete();
}

// ----------------------------------------------------------------- HA discovery
static void haSensor(const char *key, const char *name, const char *unit,
                     const char *devclass, const char *tmpl, bool diagnostic) {
  char topic[160];
  snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_%s/config", DEV_ID, key);

  JsonDocument d;
  d["name"] = name;
  d["uniq_id"] = String(DEV_ID) + "_" + key;
  d["stat_t"] = tDiag;
  d["val_tpl"] = tmpl;
  d["avty_t"] = tStatus;
  d["pl_avail"] = "online";
  d["pl_not_avail"] = "offline";
  if (unit && *unit)         d["unit_of_meas"] = unit;
  if (devclass && *devclass) d["dev_cla"] = devclass;
  if (diagnostic)            d["ent_cat"] = "diagnostic";
  JsonObject dev = d["dev"].to<JsonObject>();
  dev["ids"][0] = DEV_ID;
  dev["name"] = DEV_NAME;
  dev["mf"] = "DIY";
  dev["mdl"] = "ESP32-S3 Feather + MAX3421E";
  dev["sw"] = FW_VERSION;

  char out[768];
  size_t n = serializeJson(d, out, sizeof(out));
  mqtt.publish(topic, (const uint8_t *)out, n, true);
}

static void haBinary(const char *key, const char *name, const char *devclass,
                     const char *tmpl) {
  char topic[160];
  snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s_%s/config", DEV_ID, key);

  JsonDocument d;
  d["name"] = name;
  d["uniq_id"] = String(DEV_ID) + "_" + key;
  d["stat_t"] = tDiag;
  d["val_tpl"] = tmpl;
  d["pl_on"] = "true";
  d["pl_off"] = "false";
  d["avty_t"] = tStatus;
  d["pl_avail"] = "online";
  d["pl_not_avail"] = "offline";
  if (devclass && *devclass) d["dev_cla"] = devclass;
  d["ent_cat"] = "diagnostic";
  JsonObject dev = d["dev"].to<JsonObject>();
  dev["ids"][0] = DEV_ID;
  dev["name"] = DEV_NAME;
  dev["mf"] = "DIY";
  dev["mdl"] = "ESP32-S3 Feather + MAX3421E";
  dev["sw"] = FW_VERSION;

  char out[768];
  size_t n = serializeJson(d, out, sizeof(out));
  mqtt.publish(topic, (const uint8_t *)out, n, true);
}


static void haUps() {
  struct S { const char *k,*n,*u,*dc,*t; };
  static const S sensors[] = {
    {"capacity","UPS battery","%","battery","{{ value_json.capacity }}"},
    {"runtime","UPS runtime","s","duration","{{ value_json.runtime_s }}"},
    {"load","UPS load","%","power_factor","{{ value_json.load_pct }}"},
    {"input_v","UPS input voltage","V","voltage","{{ value_json.input_v }}"},
    {"battery_v","UPS battery voltage","V","voltage","{{ value_json.battery_v }}"},
    {"test","UPS self-test",NULL,NULL,"{{ value_json.test }}"},
  };
  for (auto &x : sensors) {
    char topic[160];
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_%s/config", DEV_ID, x.k);
    JsonDocument d;
    d["name"] = x.n; d["uniq_id"] = String(DEV_ID) + "_" + x.k;
    d["stat_t"] = tUps; d["val_tpl"] = x.t;
    d["avty_t"] = tStatus; d["pl_avail"]="online"; d["pl_not_avail"]="offline";
    if (x.u)  d["unit_of_meas"] = x.u;
    if (x.dc) d["dev_cla"] = x.dc;
    JsonObject dev = d["dev"].to<JsonObject>();
    dev["ids"][0]=DEV_ID; dev["name"]=DEV_NAME; dev["mf"]="DIY";
    dev["mdl"]="ESP32-S3 Feather + MAX3421E"; dev["sw"]=FW_VERSION;
    char out[768]; size_t n = serializeJson(d, out, sizeof(out));
    mqtt.publish(topic, (const uint8_t*)out, n, true);
  }
  struct B { const char *k,*n,*dc,*t; };
  static const B bins[] = {
    {"ac_present","UPS on mains","power","{{ value_json.ac_present }}"},
    {"discharging","UPS on battery","battery_charging","{{ value_json.discharging }}"},
    {"need_replacement","UPS battery needs replacement","problem","{{ value_json.need_replacement }}"},
    {"overload","UPS overload","problem","{{ value_json.overload }}"},
    {"shutdown_imminent","UPS shutdown imminent","problem","{{ value_json.shutdown_imminent }}"},
    {"below_cap_limit","UPS below capacity limit","problem","{{ value_json.below_cap_limit }}"},
  };
  for (auto &x : bins) {
    char topic[160];
    snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s_%s/config", DEV_ID, x.k);
    JsonDocument d;
    d["name"]=x.n; d["uniq_id"]=String(DEV_ID)+"_"+x.k;
    d["stat_t"]=tUps; d["val_tpl"]=x.t; d["pl_on"]="true"; d["pl_off"]="false";
    d["avty_t"]=tStatus; d["pl_avail"]="online"; d["pl_not_avail"]="offline";
    if (x.dc) d["dev_cla"]=x.dc;
    JsonObject dev = d["dev"].to<JsonObject>();
    dev["ids"][0]=DEV_ID; dev["name"]=DEV_NAME; dev["mf"]="DIY";
    dev["mdl"]="ESP32-S3 Feather + MAX3421E"; dev["sw"]=FW_VERSION;
    char out[768]; size_t n = serializeJson(d, out, sizeof(out));
    mqtt.publish(topic, (const uint8_t*)out, n, true);
  }
  {
    char topic[160];
    snprintf(topic, sizeof(topic), "homeassistant/select/%s_sensitivity/config", DEV_ID);
    JsonDocument d;
    d["name"]="UPS input sensitivity"; d["uniq_id"]=String(DEV_ID)+"_sensitivity";
    d["stat_t"]=tUps; d["val_tpl"]="{{ value_json.sensitivity | default('unknown') }}";
    d["cmd_t"]=tCmd; d["cmd_tpl"]="sens_{{ value }}";
    d["options"][0]="low"; d["options"][1]="medium"; d["options"][2]="high";
    d["ent_cat"]="config"; d["icon"]="mdi:sine-wave";
    d["avty_t"]=tStatus; d["pl_avail"]="online"; d["pl_not_avail"]="offline";
    JsonObject dev = d["dev"].to<JsonObject>();
    dev["ids"][0]=DEV_ID; dev["name"]=DEV_NAME; dev["mf"]="DIY";
    dev["mdl"]="ESP32-S3 Feather + MAX3421E"; dev["sw"]=FW_VERSION;
    char out[768]; size_t n = serializeJson(d, out, sizeof(out));
    mqtt.publish(topic, (const uint8_t*)out, n, true);
  }
  logf("HA UPS discovery published");
}

static void publishDiscovery() {
  haSensor("rssi", "WiFi RSSI", "dBm", "signal_strength", "{{ value_json.rssi }}", true);
  haSensor("uptime", "Uptime", "s", "duration", "{{ value_json.uptime_s }}", true);
  haSensor("heap", "Free heap", "B", "data_size", "{{ value_json.heap_free }}", true);
  haSensor("batt_v", "Backup battery voltage", "V", "voltage", "{{ value_json.batt_v }}", true);
  haSensor("batt_pct", "Backup battery", "%", "battery", "{{ value_json.batt_pct }}", true);
  haSensor("usb_id", "USB device", "", "", "{{ value_json.usb_id }}", true);
  haBinary("usb_attached", "USB device attached", "connectivity", "{{ value_json.usb_attached }}");
  haBinary("hid_mounted", "HID interface mounted", "connectivity", "{{ value_json.hid_mounted }}");
  haUps();
  logf("HA discovery published");
}

// ----------------------------------------------------------------- diagnostics
// Fills every diagnostic field. Does NOT reset the timing accumulators; the
// caller decides that, so a web poll cannot steal the worst-case numbers that
// the 15 s MQTT diag exists to report.
static void fillDiag(JsonDocument &d) {
  d["fw"]          = FW_VERSION;
  d["uptime_s"]    = (millis() - bootMs) / 1000;
  d["rssi"]        = WiFi.RSSI();
  d["ip"]          = WiFi.localIP().toString();
  d["hostname"]    = WiFi.getHostname();
  d["heap_free"]   = ESP.getFreeHeap();
  d["heap_min"]    = ESP.getMinFreeHeap();
  d["usb_attached"] = usbAttached ? "true" : "false";
  d["hid_mounted"]  = hidMounted ? "true" : "false";
  d["hid_desc_len"] = hidDescLen;
  d["wing_present"]  = wingPresent ? "true" : "false";
  d["m3421_rev"]     = m3421Rev;
  d["osc_ms"]        = m3421OscMs;
  d["poll_timeouts"] = pollTimeouts;
  d["submit_fails"]  = submitFails;
  d["ep0_aborts"]    = ep0Aborts;
  d["bus_resets"]    = busResets;
  d["host_reinits"]  = hostReinits;
  d["usb_lost_s"]    = usbLostSince ? (long)((millis() - usbLostSince) / 1000) : 0L;
  d["t_loop_ms"] = tmLoop;  d["t_usb_ms"] = tmUsb;  d["t_ota_ms"] = tmOta;
  d["t_mqtt_ms"] = tmMqtt;  d["t_poll_ms"] = tmPoll;  d["t_http_ms"] = tmHttp;
  d["ups_valid"]    = ups.valid ? "true" : "false";
  // Seconds since the last ups publish; -1 = never. THIS is the liveness signal --
  // ups_valid stays true forever once the first report lands.
  d["ups_age_s"]    = lastUpsPub ? (long)((millis() - lastUpsPub) / 1000) : -1L;
  d["mqtt"]         = mqtt.connected() ? "true" : "false";

  char idbuf[16] = "none";
  if (usbAttached) snprintf(idbuf, sizeof(idbuf), "%04x:%04x", usbVid, usbPid);
  d["usb_id"] = idbuf;

  if (fuelGaugeOk) {
    d["batt_v"]   = serialized(String(fuelGauge.cellVoltage(), 3));
    d["batt_pct"] = serialized(String(fuelGauge.cellPercent(), 1));
  } else {
    d["batt_v"]   = nullptr;
    d["batt_pct"] = nullptr;
  }

}

static void publishDiag() {
  JsonDocument d;
  fillDiag(d);
  tmLoop = tmUsb = tmOta = tmMqtt = tmPoll = tmHttp = 0;   // worst-case window closes here
  char out[640];
  size_t n = serializeJson(d, out, sizeof(out));
  mqtt.publish(tDiag, (const uint8_t *)out, n, false);
}

// ----------------------------------------------------------------- MQTT
static void onMqtt(char *topic, byte *payload, unsigned int len) {
  String cmd; for (unsigned i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();
  logf("cmd: %s", cmd.c_str());
  if (cmd == "dump")        publishDescriptor();
  else if (cmd == "diag")   publishDiag();
  else if (cmd == "discovery") publishDiscovery();
  else if (cmd == "reboot") { mqtt.publish(tStatus, "offline", true); delay(200); ESP.restart(); }
  else if (cmd == "selftest")      logf("self-test quick: %s", upsTest(1) ? "submitted" : "submit FAILED");
  else if (cmd == "selftest_deep") logf("self-test deep: %s",  upsTest(2) ? "submitted" : "submit FAILED");
  else if (cmd == "selftest_abort")logf("self-test abort: %s", upsTest(3) ? "submitted" : "submit FAILED");
  else if (cmd == "sens_low")    logf("sensitivity low: %s",    upsSetSens(0) ? "submitted" : "submit FAILED");
  else if (cmd == "sens_medium") logf("sensitivity medium: %s", upsSetSens(1) ? "submitted" : "submit FAILED");
  else if (cmd == "sens_high")   logf("sensitivity high: %s",   upsSetSens(2) ? "submitted" : "submit FAILED");
  else if (cmd == "raw33") { rawDump = true; logf("will dump raw report 33"); }
}

static void mqttConnect() {
  if (millis() - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = millis();
  if (!mqtt.connect(DEV_ID, nullptr, nullptr, tStatus, 0, true, "offline")) return;
  mqtt.publish(tStatus, "online", true);
  mqtt.subscribe(tCmd);
  publishDiscovery();
  publishDiag();
  if (hidDescLen) publishDescriptor();
  Serial.printf("MQTT connected to %s:%d\n", MQTT_HOST, MQTT_PORT);
}

// ----------------------------------------------------------------- TinyUSB callbacks
extern "C" {

void tuh_mount_cb(uint8_t daddr) {
  usbDaddr = daddr;
  usbAttached = true;
  tuh_vid_pid_get(daddr, &usbVid, &usbPid);
  Serial.printf("[usb] attached addr=%u  %04x:%04x\n", daddr, usbVid, usbPid);
}

void tuh_umount_cb(uint8_t daddr) {
  (void)daddr;
  usbAttached = false;
  hidMounted = false;
  hidDescLen = 0;
  usbVid = usbPid = 0;
  Serial.printf("[usb] detached addr=%u\n", daddr);
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx,
                      uint8_t const *report_desc, uint16_t desc_len) {
  hidDaddr = daddr; hidIdx = idx; hidMounted = true; everMounted = true;
  usbLostSince = 0; usbHealStep = 0;
  if (desc_len == 0 || report_desc == NULL) {
    // TinyUSB skips descriptors larger than CFG_TUH_ENUMERATION_BUFSIZE (256).
    // APC descriptors exceed that, so fetch it ourselves into a bigger buffer.
    hidDescLen = 0;
    hidDescWanted = true;
    Serial.println("[hid] descriptor skipped by stack (>256B) - will fetch manually");
  } else {
    hidDescLen = desc_len;
    uint16_t n = desc_len > sizeof(hidDesc) ? sizeof(hidDesc) : desc_len;
    memcpy(hidDesc, report_desc, n);
    hidDescDirty = true;
  }
  Serial.printf("[hid] mounted daddr=%u idx=%u desc_len=%u\n", daddr, idx, desc_len);
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
  (void)daddr; (void)idx;
  hidMounted = false;
  Serial.println("[hid] unmounted");
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx,
                                uint8_t const *report, uint16_t len) {
  (void)daddr; (void)idx; (void)report; (void)len;
  // v0.1: not consuming interrupt reports yet
}

} // extern "C"



static const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXT_RESET";
    case ESP_RST_SW:       return "SW_RESTART";
    case ESP_RST_PANIC:    return "PANIC (crash)";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "OTHER_WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power)";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "UNKNOWN";
  }
}

// Boot-stage beacon: a headless board must be able to say how far it got.
// Published retained, so the last successful stage survives a hang.
static void stage(const char *s) {
  Serial.printf("[stage] %s\n", s);
  if (mqtt.connected()) { mqtt.publish(tStage, s, true); mqtt.loop(); }
}

// Blocking-with-timeout connect used during setup only.
static bool mqttConnectBlocking(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (mqtt.connected()) return true;
    if (mqtt.connect(DEV_ID, nullptr, nullptr, tStatus, 0, true, "offline")) {
      mqtt.publish(tStatus, "online", true);
      mqtt.subscribe(tCmd);
      return true;
    }
    delay(500);
  }
  return false;
}


// Manual report-descriptor fetch (async; a sync call from loop() would deadlock
// the USB stack, since loop() is also what services USBHost.task()).
static void descFetchDone(tuh_xfer_t *xfer) {
  hidDescInFlight = false;
  if (xfer->result == XFER_RESULT_SUCCESS) {
    hidDescLen = (uint16_t)xfer->actual_len;
    hidDescDirty = true;
    hidDescWanted = false;
    Serial.printf("[hid] fetched report descriptor: %u bytes\n", hidDescLen);
  } else {
    Serial.printf("[hid] descriptor fetch failed, result=%d\n", (int)xfer->result);
  }
}

static void hidDescFetchTick() {
  if (!hidDescWanted || hidDescInFlight || !hidMounted) return;
  tuh_itf_info_t info;
  if (!tuh_hid_itf_get_info(hidDaddr, hidIdx, &info)) return;
  hidDescInFlight = true;
  bool ok = tuh_descriptor_get_hid_report(hidDaddr, info.desc.bInterfaceNumber,
                                          0x22 /* HID report descriptor */, 0,
                                          hidDesc, sizeof(hidDesc),
                                          descFetchDone, 0);
  if (!ok) { hidDescInFlight = false; }
  else     { Serial.println("[hid] descriptor fetch requested"); }
}


// ----------------------------------------------------------------- UPS polling
// Report map derived from this unit's own 1049-byte descriptor (tools/parse_hid.py).
// tuh_hid_get_report() is ASYNC: one request in flight, completion arrives in
// tuh_hid_get_report_complete_cb(), then we advance to the next report.
struct PollItem { uint8_t id; uint8_t len; };
static const PollItem POLL[] = {
  {22, 2},   // PresentStatus bitfield (11 flags)
  {12, 3},   // RemainingCapacity(8) + RunTimeToEmpty(16)
  {80, 1},   // PercentLoad %
  {33, 1},   // Test result 0..6
  {49, 2},   // input Voltage
  { 9, 2},   // battery Voltage
  {24, 1},   // AudibleAlarmControl 1..3
  {53, 1},   // APC:0x61 = APCSensitivity 0..2 (NUT apc-hid: low/medium/high)
};
static const uint8_t POLL_N = sizeof(POLL) / sizeof(POLL[0]);

static uint8_t  pollIdx = 0;
static bool     pollInFlight = false;
static uint32_t lastPoll = 0;
static uint32_t pollStart = 0;     // for the in-flight watchdog
static uint8_t  pollBuf[16];


static const char *testStr(int t) {
  switch (t) {
    case 1: return "passed";      case 2: return "warning";
    case 3: return "error";       case 4: return "aborted";
    case 5: return "in progress"; case 6: return "no test initiated";
    case 7: return "scheduled";   default: return "unknown";
  }
}

static const char *sensStr(int v) {
  switch (v) { case 0: return "low"; case 1: return "medium"; case 2: return "high"; default: return "unknown"; }
}

// GET_REPORT may or may not echo the report id as byte 0 -- handle both.
static const uint8_t *payload(const uint8_t *b, uint16_t len, uint8_t id, uint16_t *outLen) {
  if (len && b[0] == id) { *outLen = len - 1; return b + 1; }
  *outLen = len; return b;
}

static void applyReport(uint8_t id, const uint8_t *b, uint16_t len) {
  uint16_t n; const uint8_t *d = payload(b, len, id, &n);
  upsDirty = true;
  lastApplied = millis();
  if (rawDump && id == 33) {
    char m[80]; int o = snprintf(m, sizeof(m), "raw33 len=%u:", len);
    for (uint16_t i = 0; i < len && o < 70; i++) o += snprintf(m+o, sizeof(m)-o, " %02x", b[i]);
    Serial.println(m);
    if (mqtt.connected()) mqtt.publish(tLog, m);
    rawDump = false;
  }
  switch (id) {
    case 22: if (n >= 2) {
        uint16_t f = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
        ups.charging         = f & (1u << 0);
        ups.discharging      = f & (1u << 1);
        ups.acPresent        = f & (1u << 2);
        ups.batteryPresent   = f & (1u << 3);
        ups.belowCapLimit    = f & (1u << 4);
        ups.shutdownImminent = f & (1u << 5);
        ups.timeLimitExpired = f & (1u << 6);
        ups.commLost         = f & (1u << 7);
        ups.needReplacement  = f & (1u << 8);
        ups.overload         = f & (1u << 9);
        ups.voltNotRegulated = f & (1u << 10);
        ups.valid = true;
      } break;
    case 12: if (n >= 3) { ups.capacity = d[0];
                           ups.runtime  = (long)d[1] | ((long)d[2] << 8); } break;
    case 80: if (n >= 1) ups.load    = d[0]; break;
    case 33: if (n >= 1) ups.test    = d[0]; break;
    case 49: if (n >= 2) ups.vin     = (long)d[0] | ((long)d[1] << 8); break;
    case  9: if (n >= 2) ups.vbatt   = (long)d[0] | ((long)d[1] << 8); break;
    case 24: if (n >= 1) ups.alarm   = d[0]; break;
    case 53: if (n >= 1) ups.sens    = d[0]; break;
    default: break;
  }
}

static void fillUps(JsonVariant d) {
  d["ac_present"]        = ups.acPresent        ? "true" : "false";
  d["battery_present"]   = ups.batteryPresent   ? "true" : "false";
  d["charging"]          = ups.charging         ? "true" : "false";
  d["discharging"]       = ups.discharging      ? "true" : "false";
  d["need_replacement"]  = ups.needReplacement  ? "true" : "false";
  d["overload"]          = ups.overload         ? "true" : "false";
  d["shutdown_imminent"] = ups.shutdownImminent ? "true" : "false";
  d["below_cap_limit"]   = ups.belowCapLimit    ? "true" : "false";
  d["comm_lost"]         = ups.commLost         ? "true" : "false";
  d["volt_not_regulated"]= ups.voltNotRegulated ? "true" : "false";
  if (ups.capacity >= 0) d["capacity"]    = ups.capacity;
  if (ups.runtime  >= 0) d["runtime_s"]   = ups.runtime;
  if (ups.load     >= 0) d["load_pct"]    = ups.load;
  if (ups.test     >= 0) { d["test_code"] = ups.test; d["test"] = testStr(ups.test); }
  if (ups.vin      >= 0) d["input_v"]     = ups.vin;
  if (ups.vbatt    >= 0) { d["battery_v_raw"] = ups.vbatt;
                           d["battery_v"] = serialized(String(ups.vbatt / 100.0, 2)); }
  if (ups.alarm    >= 0) d["alarm_ctl"]   = ups.alarm;
  if (ups.sens     >= 0) { d["sens_code"] = ups.sens; d["sensitivity"] = sensStr(ups.sens); }
}

static void publishUps() {
  if (!ups.valid) return;
  JsonDocument d;
  fillUps(d);
  char out[640];
  size_t n = serializeJson(d, out, sizeof(out));
  mqtt.publish(tUps, (const uint8_t *)out, n, false);
}

// ----------------------------------------------------------------- web status
// A status page served by the device itself. Everything is inline -- no CDN, no
// external fonts, no frameworks. These boards sit on a LAN that may have no
// route to the internet, and a status page that needs the internet to render is
// useless exactly when you most want it.
//
// Accessibility note, and it is not decoration: colour is NEVER the only signal.
// Every state also carries a word (OK / FAULT / ON BATTERY), and "good" is
// emerald rather than a lime green, because red-against-green is not a
// distinction every reader can make.
static const char STATUS_HTML[] PROGMEM = R"RAWHTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>UPS Bridge</title><style>
:root{--bg:#f6f7f9;--card:#fff;--fg:#1a1d21;--mut:#606770;--line:#e3e6ea;
      --ok:#10b981;--warn:#f59e0b;--bad:#ef4444;--accent:#2563eb}
@media(prefers-color-scheme:dark){:root{--bg:#14171a;--card:#1d2125;--fg:#e8eaed;
      --mut:#9aa0a6;--line:#2c3136}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--fg);
     font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:760px;margin:0 auto}
h1{font-size:19px;margin:0 0 2px}
.sub{color:var(--mut);font-size:13px;margin-bottom:14px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;
      padding:14px;margin-bottom:12px}
.banner{display:flex;align-items:center;gap:10px;font-weight:650;font-size:17px;
        padding:14px;border-radius:10px;margin-bottom:12px;border:1px solid}
.dot{width:11px;height:11px;border-radius:50%;flex:0 0 auto}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(118px,1fr));gap:10px}
.m{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:9px 11px}
.m .k{color:var(--mut);font-size:11px;text-transform:uppercase;letter-spacing:.04em}
.m .v{font-size:21px;font-weight:650;margin-top:3px}
.m .v small{font-size:12px;font-weight:400;color:var(--mut)}
.m .v.sm{font-size:14px;font-weight:600;line-height:1.3}
h2{font-size:13px;text-transform:uppercase;letter-spacing:.05em;color:var(--mut);
   margin:0 0 9px;font-weight:600}
table{width:100%;border-collapse:collapse;font-size:13px}
td{padding:5px 0;border-bottom:1px solid var(--line)}
td:first-child{color:var(--mut)}td:last-child{text-align:right;font-variant-numeric:tabular-nums}
tr:last-child td{border-bottom:0}
.tag{display:inline-block;padding:2px 8px;border-radius:999px;font-size:11px;
     font-weight:650;letter-spacing:.03em}
.f-ok{background:color-mix(in srgb,var(--ok) 16%,transparent);color:var(--ok)}
.f-bad{background:color-mix(in srgb,var(--bad) 16%,transparent);color:var(--bad)}
.f-warn{background:color-mix(in srgb,var(--warn) 18%,transparent);color:var(--warn)}
.f-neut{background:color-mix(in srgb,var(--mut) 16%,transparent);color:var(--mut)}
.btns{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
button{font:inherit;font-weight:600;padding:9px 15px;border-radius:8px;cursor:pointer;
       border:1px solid var(--line);background:var(--bg);color:var(--fg)}
button:hover:not(:disabled){border-color:var(--accent)}
button:disabled{opacity:.45;cursor:not-allowed}
button.pri{background:var(--accent);border-color:var(--accent);color:#fff}
#msg{font-size:13px;color:var(--mut);margin-top:9px;min-height:18px}
.foot{color:var(--mut);font-size:12px;text-align:center;margin-top:4px}
.foot a{color:var(--accent)}
</style></head><body><div class="wrap">
<h1 id="name">UPS Bridge</h1><div class="sub" id="sub">connecting...</div>
<div class="banner" id="banner"><span class="dot"></span><span id="bantext">Loading</span></div>
<div class="card"><h2>UPS</h2><div class="grid" id="metrics"></div>
<div class="btns">
 <button class="pri" id="bq" onclick="run('quick')">Run self-test</button>
 <button id="bd" onclick="run('deep')">Deep test</button>
 <button id="ba" onclick="run('abort')">Abort</button>
</div><div id="msg"></div></div>
<div class="card"><h2>Status flags</h2><table id="flags"></table></div>
<div class="card"><h2>Bridge</h2><table id="dev"></table></div>
<div class="foot">refreshes every 5s &middot; <a href="/api">/api</a></div>
</div><script>
const F=[["ac_present","On mains","good"],["discharging","On battery","warn"],
 ["charging","Charging","info"],["battery_present","Battery present","good"],
 ["need_replacement","Battery needs replacement","fault"],["overload","Overload","fault"],
 ["shutdown_imminent","Shutdown imminent","fault"],["below_cap_limit","Below capacity limit","fault"],
 ["comm_lost","Comms lost","fault"],["volt_not_regulated","Voltage not regulated","fault"]];
function tagOf(kind,on){
 if(kind==="fault")return on?["FAULT","f-bad"]:["OK","f-ok"];
 if(kind==="warn") return on?["YES","f-warn"]:["NO","f-ok"];
 if(kind==="good") return on?["YES","f-ok"]:["NO","f-bad"];
 return on?["YES","f-ok"]:["NO","f-neut"];}
const T=(b)=>String(b)==="true";
function dur(s){if(s==null)return"--";s=+s;const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),
 m=Math.floor(s%3600/60);if(d)return d+"d "+h+"h";if(h)return h+"h "+m+"m";
 return m?m+"m":s+"s";}
function met(k,v,u,sm){return '<div class="m"><div class="k">'+k+'</div><div class="v'+
 (sm?" sm":"")+'">'+v+(u?' <small>'+u+'</small>':'')+'</div></div>';}
function row(k,v){return "<tr><td>"+k+"</td><td>"+v+"</td></tr>";}
async function tick(){
 let d;try{d=await (await fetch("/api",{cache:"no-store"})).json();}
 catch(e){document.getElementById("sub").textContent="unreachable";return;}
 const u=d.ups||{};
 document.getElementById("name").textContent=d.unit_name||"UPS Bridge";
 document.getElementById("sub").textContent=
   (d.unit_id||"")+" · "+(d.hostname||"")+" · "+(d.ip||"")+" · fw "+(d.fw||"");
 const faults=F.filter(f=>f[2]==="fault"&&T(u[f[0]])).map(f=>f[1]);
 const onBatt=T(u.discharging)||!T(u.ac_present);
 const b=document.getElementById("banner"),dot=b.querySelector(".dot");
 let col,txt;
 if(d.ups_valid!=="true"){col="var(--mut)";txt="No UPS data";}
 else if(faults.length){col="var(--bad)";txt="FAULT — "+faults.join(", ");}
 else if(onBatt){col="var(--warn)";txt="ON BATTERY";}
 else{col="var(--ok)";txt="OK — on mains";}
 b.style.color=col;b.style.borderColor=col;
 b.style.background="color-mix(in srgb,"+col+" 12%,transparent)";
 dot.style.background=col;document.getElementById("bantext").textContent=txt;
 const noUps=d.ups_valid!=="true"||d.hid_mounted!=="true";
 ["bq","bd","ba"].forEach(id=>document.getElementById(id).disabled=noUps);
 document.getElementById("metrics").innerHTML=
   met("Battery",u.capacity!=null?u.capacity:"--","%")+
   met("Runtime",dur(u.runtime_s),"")+
   met("Load",u.load_pct!=null?u.load_pct:"--","%")+
   met("Input",u.input_v!=null?u.input_v:"--","V")+
   met("Batt volts",u.battery_v!=null?u.battery_v:"--","V")+
   met("Self-test",u.test||"--","",1);
 document.getElementById("flags").innerHTML=F.map(f=>{
   const [lbl,cls]=tagOf(f[2],T(u[f[0]]));
   return row(f[1],'<span class="tag '+cls+'">'+lbl+"</span>");}).join("");
 document.getElementById("dev").innerHTML=
   row("Uptime",dur(d.uptime_s))+row("WiFi RSSI",(d.rssi!=null?d.rssi+" dBm":"--"))+
   row("MQTT",d.mqtt==="true"?'<span class="tag f-ok">UP</span>':'<span class="tag f-bad">DOWN</span>')+
   row("USB Host wing",d.wing_present==="true"?'<span class="tag f-ok">PRESENT</span>':'<span class="tag f-bad">NOT DETECTED</span>')+
   row("USB device",d.usb_id||"--")+
   row("HID mounted",d.hid_mounted==="true"?'<span class="tag f-ok">YES</span>':'<span class="tag f-bad">NO</span>')+
   row("HID descriptor",(d.hid_desc_len||0)+" B")+
   row("Poll timeouts",d.poll_timeouts)+
   row("Free heap",((d.heap_free||0)/1024).toFixed(1)+" KB (min "+((d.heap_min||0)/1024).toFixed(1)+")")+
   row("Loop worst",(d.t_loop_ms||0)+" ms")+
   row("Backup cell",(d.batt_v!=null?d.batt_v+" V":"--"));
}
const WARN={quick:"Run a self-test?\n\nThe UPS switches to its inverter for a few seconds. Anything on its battery outlets runs from the battery while it does.",
 deep:"Run a DEEP self-test?\n\nThis runs the UPS on battery until it is substantially discharged, then recharges for hours. Everything on the battery outlets runs from the inverter throughout.",
 abort:"Abort the self-test in progress?"};
async function run(mode){
 if(!confirm(WARN[mode]))return;
 const m=document.getElementById("msg");m.textContent="submitting...";
 try{
  const r=await fetch("/test?mode="+mode,{method:"POST"});
  const j=await r.json().catch(()=>({}));
  m.textContent=r.ok?(mode==="abort"?"Abort submitted.":"Test submitted \u2014 result appears below within a minute.")
                    :("Failed: "+(j.err||r.status));
 }catch(e){m.textContent="Failed: "+e;}
 setTimeout(tick,1500);
}
tick();setInterval(tick,5000);
</script></body></html>)RAWHTML";

static void handleRoot() {
  http.sendHeader("Cache-Control", "no-store");
  http.send_P(200, "text/html", STATUS_HTML);
}

// POST only, deliberately. A self-test transfers the UPS to its inverter for a
// few seconds, so everything on its battery outlets rides on the inverter while
// it runs -- that must never be reachable by a GET that a browser prefetch, a
// crawler or a link preview could fire on its own.
static void handleTest() {
  const String m = http.arg("mode");
  const uint8_t mode = (m == "quick") ? 1 : (m == "deep") ? 2 : (m == "abort") ? 3 : 0;
  if (!mode) { http.send(400, "application/json", "{\"ok\":false,\"err\":\"bad mode\"}"); return; }
  if (!hidMounted || !ups.valid) {
    http.send(409, "application/json", "{\"ok\":false,\"err\":\"no UPS attached\"}");
    return;
  }
  const bool ok = upsTest(mode);
  logf("self-test %s via web: %s", m.c_str(), ok ? "submitted" : "submit FAILED");
  char out[96];
  snprintf(out, sizeof(out), "{\"ok\":%s,\"mode\":\"%s\"}", ok ? "true" : "false", m.c_str());
  http.send(ok ? 200 : 502, "application/json", out);
}

static void handleApi() {
  static char out[2048];
  JsonDocument d;
  fillDiag(d);                 // shared with the MQTT diag, and does not reset timers
  d["unit_id"]   = DEV_ID;
  d["unit_name"] = DEV_NAME;
  JsonObject u = d["ups"].to<JsonObject>();
  if (ups.valid) fillUps(u);
  size_t n = serializeJson(d, out, sizeof(out));
  http.sendHeader("Cache-Control", "no-store");
  http.send(200, "application/json", out);
  (void)n;
}

// Write to the Test field (report 33, usage 0x84:0x58).
//   1 = quick test, 2 = deep test, 3 = abort
// This transfers the UPS to its inverter for a few seconds -- anything on the
// battery-backed outlets rides on the inverter while it runs.
// Write the Test field (report 33 / usage 0x84:0x58): 1 quick, 2 deep, 3 abort.
//
// The report id MUST be prefixed to the payload. TinyUSB already puts it in
// wValue, which is what the HID spec says, and the transfer is ACCEPTED either
// way -- but this APC firmware silently ignores a payload-only write and does
// nothing. Verified: payload-only never starts a test, id-prefixed always does.
// Consistent with GET_REPORT on this device returning "21 06" (id, then value).
static bool upsTest(uint8_t mode) {
  if (!hidMounted) return false;
  setBuf[0] = 33;
  setBuf[1] = mode;
  bool ok = tuh_hid_set_report(hidDaddr, hidIdx, 33, HID_REPORT_TYPE_FEATURE, setBuf, 2);
  Serial.printf("[test] mode=%u submit=%s\n", mode, ok ? "ok" : "FAILED");
  return ok;
}

// Write APCSensitivity (report 53, usage 0xFF86:0x61): 0 low, 1 medium, 2 high.
// "Low" widens the input window the UPS accepts before going to battery, which
// matters on a Powerwall island (2026-10-09: aquarium unit refused island power
// for 5 min at 120 V). Same id-prefix rule as upsTest().
static bool upsSetSens(uint8_t v) {
  if (!hidMounted || v > 2) return false;
  setBuf[0] = 53;
  setBuf[1] = v;
  return tuh_hid_set_report(hidDaddr, hidIdx, 53, HID_REPORT_TYPE_FEATURE, setBuf, 2);
}

extern "C" void tuh_hid_set_report_complete_cb(uint8_t daddr, uint8_t idx,
                                               uint8_t report_id, uint8_t report_type,
                                               uint16_t len) {
  (void)daddr; (void)idx; (void)report_type;
  char m[96];
  snprintf(m, sizeof(m), "set_report complete: id=%u len=%u (%s)",
           report_id, len, len ? "ACCEPTED" : "REJECTED/stalled");
  Serial.println(m);
  if (mqtt.connected()) mqtt.publish(tLog, m);
}

static void pollTick() {
  // A GET_REPORT whose completion callback never arrives (device re-enumerated,
  // transfer dropped) would otherwise wedge polling permanently.
  if (pollInFlight && millis() - pollStart > 2000) {
    pollInFlight = false;
    pollTimeouts++;
    pollIdx = (pollIdx + 1) % POLL_N;
    // v1.7.2: a timed-out GET_REPORT is still owned by TinyUSB. Its control
    // endpoint stays busy, every later tuh_hid_get_report() is refused, and
    // polling never makes progress again while usb_attached/hid_mounted read
    // true (seen on both units, twice). Abort the stuck control transfer so the
    // stack returns to idle; the submit-failure counter and the stale watchdog
    // in loop() cover the case where even that does not help.
    bool ab = tuh_edpt_abort_xfer(hidDaddr, 0);
    if (ab) ep0Aborts++;
    logf("[poll] report timed out (%lu total), ep0 abort %s",
         (unsigned long)pollTimeouts, ab ? "ok" : "no-op");
  }
  if (!hidMounted || pollInFlight) return;
  if (millis() - lastPoll < 1000) return;      // ~1 report/s -> full sweep ~7s
  lastPoll = millis();
  const PollItem &it = POLL[pollIdx];
  pollInFlight = true;
  pollStart = millis();
  if (!tuh_hid_get_report(hidDaddr, hidIdx, it.id, HID_REPORT_TYPE_FEATURE,
                          pollBuf, (uint16_t)(it.len + 1))) {
    pollInFlight = false;
    submitFails++;
    pollIdx = (pollIdx + 1) % POLL_N;
  }
}

extern "C" void tuh_hid_get_report_complete_cb(uint8_t daddr, uint8_t idx,
                                               uint8_t report_id, uint8_t report_type,
                                               uint16_t len) {
  (void)daddr; (void)idx; (void)report_type;
  if (len) applyReport(report_id, pollBuf, len);
  pollInFlight = false;
  pollIdx = (pollIdx + 1) % POLL_N;
  // Publishing is timer-driven in loop() -- do not tie it to the sweep wrap here.
}

// ----------------------------------------------------------------- USB recovery
static void usbBusReset() {
  tuh_rhport_reset_bus(USB_RHPORT, true);
  delay(30);
  tuh_rhport_reset_bus(USB_RHPORT, false);
  busResets++;
}

// Full host stack teardown and re-init: hcd_deinit and hcd_init both pulse
// CHIPRES on the MAX3421E, which is the closest thing to a power cycle the
// firmware can do. Does not recreate the interrupt task or the SPI bus, which
// USBHost.begin() owns and which are still fine.
static bool usbHostReinit() {
  pollInFlight = false;
  hidMounted = false; usbAttached = false;
  tuh_deinit(USB_RHPORT);
  delay(100);
  const tusb_rhport_init_t rh = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL };
  bool ok = tuh_rhport_init(USB_RHPORT, &rh);
  hostReinits++;
  return ok;
}

// ----------------------------------------------------------------- setup / loop
// ----------------------------------------------------- MAX3421E pre-flight
// TinyUSB's hcd_init() waits for the MAX3421E's crystal with an UNBOUNDED spin:
//   while( !(reg_read(USBIRQ) & OSCOK) ) { }
// If the oscillator never starts, setup() never returns. WiFi is already up by
// then, so the board still answers pings while having no HTTP, no MQTT and --
// critically -- no OTA. Only a physical power cycle recovers it.
//
// Observed in the field 2026-10-09 on the basement unit: pressing directly on
// the MAX3421E package released the spin instantly and the web server appeared.
// That is a marginal solder joint on the chip itself, and since SPI reads work
// fine (the REVISION read always succeeds) it is on a crystal or supply pin --
// exactly the subset that lets the chip talk but never oscillate.
//
// So probe the chip ourselves FIRST, with timeouts, and report what we find
// instead of disappearing. Same registers and framing the driver uses.
#define M3421_USBIRQ    (13u << 3)
#define M3421_USBCTL    (15u << 3)
#define M3421_PINCTL    (17u << 3)
#define M3421_FDUPSPI   (1u << 4)
#define M3421_REVISION  (18u << 3)
#define M3421_WRITE     0x02
#define M3421_CHIPRES   (1u << 5)
#define M3421_OSCOK     (1u << 0)

static uint8_t m3421Read(uint8_t reg) {
  digitalWrite(MAX3421_CS, LOW);
  SPI.transfer(reg);
  uint8_t v = SPI.transfer(0x00);
  digitalWrite(MAX3421_CS, HIGH);
  return v;
}

static void m3421Write(uint8_t reg, uint8_t val) {
  digitalWrite(MAX3421_CS, LOW);
  SPI.transfer(reg | M3421_WRITE);
  SPI.transfer(val);
  digitalWrite(MAX3421_CS, HIGH);
}

// Returns true if the chip answered AND its oscillator started within timeoutMs.
static bool probeMax3421(uint32_t timeoutMs) {
  pinMode(MAX3421_CS, OUTPUT);
  digitalWrite(MAX3421_CS, HIGH);
  SPI.begin();                       // board defaults; USBHost.begin() re-inits harmlessly
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

  // MUST come first. The MAX3421E powers up in HALF-duplex SPI, where MISO is
  // not driven at all -- it talks back over MOSI. Reading any register before
  // setting FDUPSPI returns 0x00 on a perfectly healthy chip. hcd_init() does
  // this on its first line for exactly this reason; omitting it here made the
  // probe report a dead chip on every COLD boot, while reading correctly after
  // an OTA or cmd reboot (which leave PINCTL set from the previous init). That
  // false negative cost a lot of pointless rework -- do not remove it.
  m3421Write(M3421_PINCTL, M3421_FDUPSPI);
  m3421Rev = m3421Read(M3421_REVISION);
  const bool revOk = (m3421Rev == 0x01 || m3421Rev == 0x12 || m3421Rev == 0x13);
  Serial.printf("[max3421] revision=0x%02X %s\n", m3421Rev, revOk ? "ok" : "BAD (no chip / SPI dead)");

  if (revOk) {
    m3421Write(M3421_USBCTL, M3421_CHIPRES);
    m3421Write(M3421_USBCTL, 0);
    const uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
      if (m3421Read(M3421_USBIRQ) & M3421_OSCOK) { m3421OscMs = (int32_t)(millis() - t0); break; }
      delay(1);
    }
    if (m3421OscMs >= 0) Serial.printf("[max3421] oscillator OK in %ld ms\n", (long)m3421OscMs);
    else Serial.printf("[max3421] oscillator DID NOT START in %lu ms -- suspect a solder joint "
                       "on the crystal or supply pins of U1\n", (unsigned long)timeoutMs);
  }
  SPI.endTransaction();
  return revOk && m3421OscMs >= 0;
}

void setup() {
  Serial.begin(115200);
  bootMs = millis();

  snprintf(tBase,   sizeof(tBase),   "apcups/%s", DEV_ID);
  snprintf(tStatus, sizeof(tStatus), "%s/status", tBase);
  snprintf(tDiag,   sizeof(tDiag),   "%s/diag",   tBase);
  snprintf(tDesc,   sizeof(tDesc),   "%s/hid/descriptor", tBase);
  snprintf(tLog,    sizeof(tLog),    "%s/log",    tBase);
  snprintf(tCmd,    sizeof(tCmd),    "%s/cmd",    tBase);
  snprintf(tStage,  sizeof(tStage),  "%s/stage",  tBase);
  snprintf(tBoot,   sizeof(tBoot),   "%s/boot",   tBase);
  snprintf(tUps,    sizeof(tUps),    "%s/ups",    tBase);

  Wire.begin();
  fuelGaugeOk = fuelGauge.begin(&Wire);

  // setHostname() only writes a static buffer; WiFiGeneric::mode() is what pushes
  // it to the netif (set_esp_interface_hostname(..., get_esp_netif_hostname())).
  // So it MUST be called before mode(), or you get the MAC-derived default.
  WiFi.setHostname(NET_HOST);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // USB host servicing dislikes modem sleep
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);

  // --- MQTT first, so later stages are observable even if they hang -------
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(MQTT_BUFFER_BYTES);
  mqtt.setKeepAlive(60);      // broker was timing us out at the 15s default
  mqtt.setSocketTimeout(5);   // default 15s read stall is longer than keepalive
  mqtt.setCallback(onMqtt);
  bool mq = mqttConnectBlocking(12000);
  Serial.printf("[boot] wifi=%s ip=%s mqtt=%s\n",
                WiFi.status() == WL_CONNECTED ? "up" : "DOWN",
                WiFi.localIP().toString().c_str(), mq ? "up" : "DOWN");
  {
    char b[160];
    snprintf(b, sizeof(b), "{\"reset\":\"%s\",\"code\":%d,\"fw\":\"%s\",\"heap\":%u}",
             resetReasonStr(esp_reset_reason()), (int)esp_reset_reason(),
             FW_VERSION, (unsigned)ESP.getFreeHeap());
    Serial.printf("[boot] %s\n", b);
    if (mq) mqtt.publish(tBoot, b, true);
  }
  stage("mqtt_up");

  // --- OTA ---------------------------------------------------------------
  ArduinoOTA.setHostname(NET_HOST);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("[ota] start"); });
  ArduinoOTA.onEnd([]()   { Serial.println("[ota] done");  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %u\n", e); });
  stage("ota_begin_enter");
  ArduinoOTA.begin();
  stage("ota_ok");

  // --- HTTP status page --------------------------------------------------
  http.on("/", handleRoot);
  http.on("/api", handleApi);
  http.on("/test", HTTP_POST, handleTest);
  http.onNotFound([]() { http.sendHeader("Location", "/"); http.send(302, "text/plain", ""); });
  http.begin();
  stage("http_ok");

  // --- USB host on the MAX3421E (roothub 1) ------------------------------
  // NOTE: if the MAX3421E answers the revision read but is then flaky -- a
  // half-seated wing is the way to achieve that -- hcd_init() spins forever in
  // its oscillator-stable wait. That hang is observable: the stage topic stops
  // at usbhost_begin_enter and never reaches usbhost_ok.
  // Probe before begin(): hcd_init() would spin forever on a dead oscillator.
  stage("usbhost_probe");
  const bool oscOk = probeMax3421(1000);
  logf("MAX3421E rev=0x%02X osc_ms=%ld", m3421Rev, (long)m3421OscMs);
  if (!oscOk) {
    // Do NOT call USBHost.begin() -- it would hang and take HTTP/MQTT/OTA with
    // it. Run degraded instead: everything else stays up and says what is wrong,
    // and the unit remains OTA-able.
    wingPresent = false;
    stage(m3421Rev ? "usbhost_NO_OSC" : "usbhost_NO_WING");
    logf("SKIPPING USB host init -- %s. Bridge stays online so this is visible.",
         m3421Rev ? "oscillator never started (press/reflow U1 on the wing)"
                  : "MAX3421E did not answer (wing missing or unseated)");
  } else {
    stage("usbhost_begin_enter");
    wingPresent = USBHost.begin(1);
    stage(wingPresent ? "usbhost_ok" : "usbhost_NO_WING");
    if (!wingPresent) logf("MAX3421E did not respond -- USB Host wing missing or unseated");
  }

  if (mq) { publishDiscovery(); publishDiag(); }
  stage("setup_done");
  Serial.printf("APC UPS bridge v%s ready\n", FW_VERSION);
}

#define TIME_IT(acc, call) { uint32_t _t = millis(); call; uint32_t _d = millis() - _t; if (_d > acc) acc = _d; }

void loop() {
  uint32_t loopStart = millis();

  TIME_IT(tmUsb, USBHost.task(0))   // 0 = non-blocking; default UINT32_MAX blocks forever
  TIME_IT(tmOta, ArduinoOTA.handle())
  TIME_IT(tmHttp, http.handleClient())

  // Verbose while offline: a headless box must explain itself over serial.
  static uint32_t lastWifiLog = 0, lastRetry = 0;
  static uint8_t  offlineTicks = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiLog >= 3000) {
      lastWifiLog = millis();
      int st = WiFi.status();
      Serial.printf("[wifi] status=%d %s  (fw %s, up %lus)\n",
                    st, wifiStatusStr(st), FW_VERSION,
                    (unsigned long)((millis() - bootMs) / 1000));
      if (++offlineTicks % 5 == 0) wifiScanReport();
    }
    if (millis() - lastRetry >= 20000) {
      lastRetry = millis();
      Serial.println("[wifi] retrying begin()");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) TIME_IT(tmMqtt, mqttConnect())
    else                   TIME_IT(tmMqtt, mqtt.loop())
  }

  hidDescFetchTick();
  TIME_IT(tmPoll, pollTick())
  if (hidDescDirty && mqtt.connected()) publishDescriptor();

  if (millis() - lastDiag >= DIAG_INTERVAL_MS) {
    lastDiag = millis();
    if (mqtt.connected()) publishDiag();
  }

  // Timer-driven UPS publish: fires as soon as data is dirty after boot, then at
  // most every UPS_PUBLISH_MS. Independent of where the poll sweep is, so no
  // sequence of timeouts or submit failures can silence it.
  if (upsDirty && ups.valid && mqtt.connected() &&
      (lastUpsPub == 0 || millis() - lastUpsPub >= UPS_PUBLISH_MS)) {
    publishUps();
    lastUpsPub = millis();
    upsDirty   = false;
  }

  // USB watchdogs (v1.7.3). Two failure shapes, one ladder each:
  //  A. device mounted but no report applied  -> ep0 abort, then host reinit,
  //     then (much later) restart.
  //  B. device missing after we once had one   -> bus reset, then host reinit
  //     every minute, then (much later) restart. A bridge that never had a
  //     device keeps trying reinit but never restarts, so a deliberately
  //     UPS-less board does not reboot-loop.
  if (wingPresent) {
    if (hidMounted) {
      uint32_t since = lastApplied ? lastApplied : bootMs;
      uint32_t stale = millis() - since;
      if (stale > 600000UL) {
        logf("[heal] no report applied for %lus after reinit -- restarting", (unsigned long)(stale / 1000));
        if (mqtt.connected()) { mqtt.loop(); delay(300); }
        ESP.restart();
      } else if (stale > 300000UL && millis() - lastHeal > 60000UL) {
        lastHeal = millis();
        bool ok = usbHostReinit();
        logf("[heal] no report applied for %lus -- host reinit %s", (unsigned long)(stale / 1000), ok ? "ok" : "FAILED");
      } else if (stale > 120000UL && millis() - lastHeal > 60000UL) {
        lastHeal = millis();
        bool ab = tuh_edpt_abort_xfer(hidDaddr, 0);
        if (ab) ep0Aborts++;
        pollInFlight = false;
        logf("[heal] no report applied for %lus -- ep0 abort %s", (unsigned long)(stale / 1000), ab ? "ok" : "no-op");
      }
    } else {
      if (!usbLostSince) usbLostSince = millis();
      uint32_t lost = millis() - usbLostSince;
      if (lost > 60000UL && millis() - lastUsbHeal > 60000UL) {
        lastUsbHeal = millis();
        usbHealStep++;
        if (usbHealStep == 1) {
          usbBusReset();
          logf("[heal] no USB device for %lus -- bus reset", (unsigned long)(lost / 1000));
        } else if (lost > 600000UL && everMounted) {
          logf("[heal] no USB device for %lus after %u reinits -- restarting", (unsigned long)(lost / 1000), (unsigned)hostReinits);
          if (mqtt.connected()) { mqtt.loop(); delay(300); }
          ESP.restart();
        } else {
          bool ok = usbHostReinit();
          logf("[heal] no USB device for %lus -- host reinit %s", (unsigned long)(lost / 1000), ok ? "ok" : "FAILED");
        }
      }
    }
  }

  uint32_t d = millis() - loopStart;
  if (d > tmLoop) tmLoop = d;
}
