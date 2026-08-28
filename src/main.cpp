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
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_MAX1704X.h>
#include "Adafruit_TinyUSB.h"
#include "SPI.h"
#include "secrets.h"
#include <esp_system.h>

// ----------------------------------------------------------------- config
static const char *FW_VERSION = "1.0.0";
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
static const uint32_t MQTT_RETRY_MS      = 5000;
static const size_t   MQTT_BUFFER_BYTES  = 8192;

// ----------------------------------------------------------------- state
Adafruit_USBH_Host USBHost(&SPI, MAX3421_CS, MAX3421_INT);
Adafruit_MAX17048  fuelGauge;
bool fuelGaugeOk = false;

WiFiClient   net;
PubSubClient mqtt(net);

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

static uint32_t lastDiag = 0, lastMqttTry = 0;
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
static void publishDiag() {
  JsonDocument d;
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

  char out[512];
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
  hidDaddr = daddr; hidIdx = idx; hidMounted = true;
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
};
static const uint8_t POLL_N = sizeof(POLL) / sizeof(POLL[0]);

static uint8_t  pollIdx = 0;
static bool     pollInFlight = false;
static uint32_t lastPoll = 0;
static uint8_t  pollBuf[16];

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
} ups;

static const char *testStr(int t) {
  switch (t) {
    case 1: return "passed";      case 2: return "warning";
    case 3: return "error";       case 4: return "aborted";
    case 5: return "in progress"; case 6: return "no test initiated";
    case 7: return "scheduled";   default: return "unknown";
  }
}

// GET_REPORT may or may not echo the report id as byte 0 -- handle both.
static const uint8_t *payload(const uint8_t *b, uint16_t len, uint8_t id, uint16_t *outLen) {
  if (len && b[0] == id) { *outLen = len - 1; return b + 1; }
  *outLen = len; return b;
}

static void applyReport(uint8_t id, const uint8_t *b, uint16_t len) {
  uint16_t n; const uint8_t *d = payload(b, len, id, &n);
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
    default: break;
  }
}

static void publishUps() {
  if (!ups.valid) return;
  JsonDocument d;
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
  char out[640];
  size_t n = serializeJson(d, out, sizeof(out));
  mqtt.publish(tUps, (const uint8_t *)out, n, false);
}

static void pollTick() {
  if (!hidMounted || pollInFlight) return;
  if (millis() - lastPoll < 1000) return;      // ~1 report/s -> full sweep ~7s
  lastPoll = millis();
  const PollItem &it = POLL[pollIdx];
  pollInFlight = true;
  if (!tuh_hid_get_report(hidDaddr, hidIdx, it.id, HID_REPORT_TYPE_FEATURE,
                          pollBuf, (uint16_t)(it.len + 1))) {
    pollInFlight = false;
    pollIdx = (pollIdx + 1) % POLL_N;
  }
}

extern "C" void tuh_hid_get_report_complete_cb(uint8_t daddr, uint8_t idx,
                                               uint8_t report_id, uint8_t report_type,
                                               uint16_t len) {
  (void)daddr; (void)idx; (void)report_type;
  if (len) applyReport(report_id, pollBuf, len);
  pollInFlight = false;
  bool wrapped = (pollIdx + 1) % POLL_N == 0;
  pollIdx = (pollIdx + 1) % POLL_N;
  if (wrapped && mqtt.connected()) publishUps();
}

// ----------------------------------------------------------------- setup / loop
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

  // --- USB host on the MAX3421E (roothub 1) ------------------------------
  stage("usbhost_begin_enter");
  USBHost.begin(1);
  stage("usbhost_ok");

  if (mq) { publishDiscovery(); publishDiag(); }
  stage("setup_done");
  Serial.printf("APC UPS bridge v%s ready\n", FW_VERSION);
}

void loop() {
  USBHost.task(0);   // 0 = non-blocking; default UINT32_MAX blocks forever
  ArduinoOTA.handle();

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
    if (!mqtt.connected()) mqttConnect();
    else                   mqtt.loop();
  }

  hidDescFetchTick();
  pollTick();
  if (hidDescDirty && mqtt.connected()) publishDescriptor();

  if (millis() - lastDiag >= DIAG_INTERVAL_MS) {
    lastDiag = millis();
    if (mqtt.connected()) publishDiag();
  }
}
