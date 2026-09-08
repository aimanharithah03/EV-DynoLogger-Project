/**
 * KC868-A2 <-> PZEM-017 RS485 Modbus RTU
 * DYNO DATA LOGGER — SPIFFS + WiFi Dashboard + Live Config
 *
 * Hardware:
 *   RS485 RXD -> GPIO 35  |  RS485 TXD -> GPIO 32
 *   Power     -> AC/DC 12V adapter + 5V supply to PZEM
 *   Serial Settings: 9600 baud | 8 data bits | 2 stop bits | No parity
 *
 * ── Libraries (Arduino Library Manager) ─────────────────────────────
 *   - ModbusMaster  (by Doc Walker)
 *   All others (SPIFFS, WiFi, WebServer, time) built into ESP32 core.
 *
 * ── Partition Scheme ─────────────────────────────────────────────────
 *   Arduino IDE → Tools → Partition Scheme →
 *   "Default 4MB with spiffs"
 *
 * ── Web Endpoints ────────────────────────────────────────────────────
 *   STA mode (joined router):  http://<DHCP-IP>/
 *   AP  mode (direct connect): http://192.168.4.1/
 *
 *   /           → Live dashboard + config panel + trend chart
 *   /download   → Download CSV (dyno_DDMMYYYY_HHMM_op_tid)
 *   /clear      → Wipe log (keeps header)
 *   /start      → Begin logging session (CSV writes resume)
 *   /stop       → Pause logging session (CSV writes paused, readings continue)
 *   /config     → POST handler for config saves
 *   /status     → JSON status
 *   /trend      → JSON Voltage/Current/Power/Energy history (chart data)
 *
 * ── Voltage Alarm ────────────────────────────────────────────────────
 *   High threshold default: 300V | Low threshold default: 7V
 *   Both editable from the Configuration panel on the dashboard.
 *   A status indicator above the live readings turns red (high) or
 *   amber (low) and pulses when voltage crosses either threshold.
 *
 * ── WiFi AP Mode ─────────────────────────────────────────────────────
 *   If STA connection fails (or WIFI_SSID is blank), the ESP32
 *   automatically creates its own WiFi hotspot:
 *     SSID    : DynoLogger
 *     Password: dyno1234
 *   Connect your phone/laptop to that network, then open
 *   http://192.168.4.1 in any browser. No router needed.
 */

#include <ModbusMaster.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <time.h>

// ─── Object declarations — must come before any function that uses them ─
HardwareSerial rs485Serial(2);
ModbusMaster   pzem;
WebServer      server(80);

// ─── WiFi Credentials (Station mode — joins existing router) ──────────
// These are FALLBACK defaults only — if /config.txt has wifiSSID/wifiPass
// entries (saved from the dashboard), those override these at boot.
// Leave them as-is if you prefer to set credentials only via dashboard.
const char* WIFI_SSID_DEFAULT     = "";
const char* WIFI_PASSWORD_DEFAULT = "";

// ─── Access Point credentials (AP mode — ESP32 becomes the hotspot) ───
// Connect your phone/PC to this network, then open http://192.168.4.1
const char* AP_SSID       = "DynoLogger";   // Network name broadcast by ESP32
const char* AP_PASSWORD   = "dyno1234";     // Min 8 chars; set "" for open network
const uint8_t AP_CHANNEL  = 6;              // WiFi channel 1–13

// ─── mDNS hostname ────────────────────────────────────────────────────
// In STA mode: access dashboard at http://dynologger.local (same network)
// In AP mode:  http://dynologger.local works on most platforms after
//              connecting to the DynoLogger hotspot (Windows may need IP)
const char* MDNS_NAME = "dynologger";

// ─── WiFi mode tracker ────────────────────────────────────────────────
enum WifiMode { MODE_NONE, MODE_STA, MODE_AP };
WifiMode wifiMode = MODE_NONE;

// ─── NTP (Malaysia UTC+8) ─────────────────────────────────────────────
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET = 8 * 3600;
const int   DST_OFFSET = 0;

// ─── Fixed Hardware Pins ──────────────────────────────────────────────
#define RS485_RX_PIN  35
#define RS485_TX_PIN  32
#define BAUD_RATE     9600

// KC868-A2 onboard relay GPIO assignments
// Relay outputs are active-LOW on this board (LOW = ON, HIGH = OFF)
#define RELAY1_PIN    15   // Relay 1 — On/Power LED (stays ON while unit is running)
#define RELAY2_PIN    2    // Relay 2 — Logging LED (ON during active logging session)

// ─── SPIFFS paths ─────────────────────────────────────────────────────
#define CSV_PATH      "/dyno_log.csv"
#define CFG_PATH      "/config.txt"
#define MEMO_PATH     "/memo.txt"
#define CSV_HEADER    "Date,Time,Voltage(V),Current(A),Power(W),Energy(kWh)\n"
#define SPIFFS_MIN_FREE 153600  // 150 KB safety floor

// ─── Trend buffer (in-memory ring buffer, RAM only — not persisted) ───
// Powers the live Voltage/Current/Power chart on the dashboard.
// Sized to hold a reasonable on-screen history without using much heap:
// 120 points × ~16 bytes/point ≈ 2 KB RAM.
#define TREND_BUF_SIZE 120

// ══════════════════════════════════════════════════════════════════════
//  RUNTIME CONFIG  (editable from dashboard, saved to SPIFFS)
// ══════════════════════════════════════════════════════════════════════
struct Config {
  String  deviceIP;        // Informational; ESP32 IP is DHCP-assigned
  uint16_t port;           // Web server port (requires reboot to change)
  uint8_t  modbusID;       // Modbus slave address (0x01–0xF7)
  uint32_t logIntervalMs;  // Logging interval in milliseconds
  String  outputFolder;    // Label only (SPIFFS has one flat namespace)
  String  operatorName;    // Used in CSV filename
  String  testID;          // Used in CSV filename
  float   highVoltThresh;  // High voltage alarm threshold (V)
  float   lowVoltThresh;   // Low voltage alarm threshold (V)
  String  wifiSSID;        // STA WiFi SSID — saved to flash, editable from dashboard
  String  wifiPass;        // STA WiFi password — saved to flash, editable from dashboard
} cfg;

// Defaults — overwritten by loadConfig() if CFG_PATH exists
void applyDefaults() {
  cfg.deviceIP       = "";
  cfg.port           = 80;
  cfg.modbusID       = 0x01;
  cfg.logIntervalMs  = 5000;
  cfg.outputFolder   = "dyno_data";
  cfg.operatorName   = "operator";
  cfg.testID         = "test01";
  cfg.highVoltThresh = 300.0f;
  cfg.lowVoltThresh  = 7.0f;
  cfg.wifiSSID       = String(WIFI_SSID_DEFAULT);
  cfg.wifiPass       = String(WIFI_PASSWORD_DEFAULT);
}

// ─── Logging session state ─────────────────────────────────────────────
// Controlled by the Start/Stop buttons on the dashboard. When stopped,
// the Modbus poll still runs (so live readings keep showing) but no
// rows are written to the CSV — lets you pause/resume a test run
// without losing the connection or restarting the device.
bool loggingActive = false;

// ─── Save config to SPIFFS (simple key=value format) ──────────────────
void saveConfig() {
  File f = SPIFFS.open(CFG_PATH, "w");
  if (!f) return;
  f.printf("port=%u\n",        cfg.port);
  f.printf("modbusID=%u\n",    cfg.modbusID);
  f.printf("logInterval=%u\n", cfg.logIntervalMs);
  f.printf("folder=%s\n",      cfg.outputFolder.c_str());
  f.printf("operator=%s\n",    cfg.operatorName.c_str());
  f.printf("testID=%s\n",      cfg.testID.c_str());
  f.printf("highVolt=%.2f\n",  cfg.highVoltThresh);
  f.printf("lowVolt=%.2f\n",   cfg.lowVoltThresh);
  f.printf("wifiSSID=%s\n",    cfg.wifiSSID.c_str());
  f.printf("wifiPass=%s\n",    cfg.wifiPass.c_str());
  f.close();
  Serial.println("[CFG] Config saved.");
}

// ─── Load config from SPIFFS ──────────────────────────────────────────
void loadConfig() {
  applyDefaults();
  if (!SPIFFS.exists(CFG_PATH)) return;
  File f = SPIFFS.open(CFG_PATH, "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    if (key == "port")        cfg.port           = val.toInt();
    if (key == "modbusID")    cfg.modbusID       = (uint8_t)val.toInt();
    if (key == "logInterval") cfg.logIntervalMs  = val.toInt();
    if (key == "folder")      cfg.outputFolder   = val;
    if (key == "operator")    cfg.operatorName   = val;
    if (key == "testID")      cfg.testID         = val;
    if (key == "highVolt")    cfg.highVoltThresh = val.toFloat();
    if (key == "lowVolt")     cfg.lowVoltThresh  = val.toFloat();
    if (key == "wifiSSID")    cfg.wifiSSID       = val;
    if (key == "wifiPass")    cfg.wifiPass       = val;
  }
  f.close();
  Serial.println("[CFG] Config loaded.");
}

// ─── Globals ──────────────────────────────────────────────────────────
// (rs485Serial, pzem, server declared at top after includes)

int           rowCount      = 0;
unsigned long lastLogTime   = 0;
unsigned long bootTime      = 0;
String        lastError     = "";   // Last Modbus error string
int           errorCount    = 0;    // Total error count this session
float         lastVoltage   = 0, lastCurrent = 0,
              lastPower     = 0, lastEnergy  = 0;
bool          lastReadOK    = false;

// ─── Relay 2 blink state (non-blocking) ───────────────────────────────
// When a Modbus error occurs, Relay 2 (Logging LED) blinks at 2-second
// intervals regardless of logging state, to alert the operator.
// Once the error clears (next successful read), the relay immediately
// returns to its normal state (ON if logging, OFF if not).
#define BLINK_INTERVAL_MS 2000
unsigned long lastBlinkTime  = 0;
bool          blinkState     = false;   // current physical relay state during blink

// ─── Voltage alarm state ────────────────────────────────────────────────
bool highVoltAlarm = false;   // True when voltage >= cfg.highVoltThresh
bool lowVoltAlarm  = false;   // True when voltage <= cfg.lowVoltThresh

// ─── Trend ring buffer ─────────────────────────────────────────────────
struct TrendPoint {
  uint32_t t;   // seconds since boot (relative; small footprint)
  float    v, i, p, e;
};
TrendPoint trendBuf[TREND_BUF_SIZE];
int trendHead  = 0;   // next write index
int trendCount = 0;   // how many valid points currently stored

// Push one reading into the ring buffer (overwrites oldest when full)
void pushTrendPoint(float v, float i, float p, float e) {
  trendBuf[trendHead].t = (millis() - bootTime) / 1000;
  trendBuf[trendHead].v = v;
  trendBuf[trendHead].i = i;
  trendBuf[trendHead].p = p;
  trendBuf[trendHead].e = e;
  trendHead = (trendHead + 1) % TREND_BUF_SIZE;
  if (trendCount < TREND_BUF_SIZE) trendCount++;
}

// ─── Modbus Callbacks ─────────────────────────────────────────────────
void preTransmission()  {}
void postTransmission() {}

// ══════════════════════════════════════════════════════════════════════
//  UTILITY
// ══════════════════════════════════════════════════════════════════════

String getDate() {
  struct tm t; if (!getLocalTime(&t)) return "N/A";
  char buf[12]; strftime(buf, sizeof(buf), "%Y-%m-%d", &t); return String(buf);
}
String getDateDMY() {
  struct tm t; if (!getLocalTime(&t)) return "00000000";
  char buf[10]; strftime(buf, sizeof(buf), "%d%m%Y", &t); return String(buf);
}
String getTimeStr() {
  struct tm t; if (!getLocalTime(&t)) return "N/A";
  char buf[10]; strftime(buf, sizeof(buf), "%H:%M:%S", &t); return String(buf);
}
String getTimeHHMM() {
  struct tm t; if (!getLocalTime(&t)) return "0000";
  char buf[6]; strftime(buf, sizeof(buf), "%H%M", &t); return String(buf);
}
String uptimeStr() {
  unsigned long s = (millis() - bootTime) / 1000;
  unsigned long h = s / 3600; s %= 3600;
  unsigned long m = s / 60;   s %= 60;
  char buf[16]; sprintf(buf, "%02luh %02lum %02lus", h, m, s);
  return String(buf);
}

// Sanitise a string for use in filenames (replace spaces/specials with _)
String sanitise(String s) {
  String out = "";
  for (int i = 0; i < (int)s.length(); i++) {
    char c = s[i];
    if (isAlphaNumeric(c)) out += c;
    else out += '_';
  }
  return out;
}

// Build CSV download filename: dyno_DDMMYYYY_HHMM_operator_testid.csv
String buildFilename() {
  return "dyno_" + getDateDMY() + "_" + getTimeHHMM() +
         "_" + sanitise(cfg.operatorName) +
         "_" + sanitise(cfg.testID) + ".csv";
}

// ══════════════════════════════════════════════════════════════════════
//  WIFI + NTP
// ══════════════════════════════════════════════════════════════════════

// ─── Station mode: join an existing router ────────────────────────────
bool startSTA() {
  if (cfg.wifiSSID.length() == 0) {
    Serial.println("[STA] No SSID configured — skipping to AP.");
    return false;
  }
  Serial.printf("Trying STA mode — SSID: %s\n", cfg.wifiSSID.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSSID.c_str(), cfg.wifiPass.c_str());
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiMode     = MODE_STA;
    cfg.deviceIP = WiFi.localIP().toString();

    // Start mDNS so the dashboard is reachable by name, not just IP
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[mDNS] Hostname registered: http://%s.local\n", MDNS_NAME);
    }

    Serial.println("\n========================================");
    Serial.println("  DYNO LOGGER — NETWORK READY (STA)");
    Serial.println("========================================");
    Serial.printf("  WiFi SSID  : %s\n", cfg.wifiSSID.c_str());
    Serial.printf("  IP Address : http://%s\n", cfg.deviceIP.c_str());
    Serial.printf("  Hostname   : http://%s.local\n", MDNS_NAME);
    Serial.println("========================================\n");
    return true;
  }
  Serial.println("\n[STA] Failed — wrong credentials or router unreachable. Falling back to AP.");
  WiFi.disconnect(true);
  return false;
}

// ─── Access Point mode: ESP32 hosts its own WiFi network ─────────────
// After connecting to AP_SSID, open http://192.168.4.1 in your browser.
// AP mode has no internet — NTP will not work, timestamps show N/A
// unless you set time manually or previously synced in STA mode.
void startAP() {
  Serial.printf("Starting AP mode — SSID: %s\n", AP_SSID);
  WiFi.mode(WIFI_AP);

  // softAP(ssid, password, channel, hidden, max_connections)
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, 4);

  if (ok) {
    wifiMode     = MODE_AP;
    cfg.deviceIP = WiFi.softAPIP().toString();  // Always 192.168.4.1

    // mDNS works on most platforms in AP mode too
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
    }

    Serial.println("\n========================================");
    Serial.println("  DYNO LOGGER — NETWORK READY (AP)");
    Serial.println("========================================");
    Serial.printf("  Hotspot SSID : %s\n",       AP_SSID);
    Serial.printf("  Password     : %s\n",        AP_PASSWORD);
    Serial.printf("  IP Address   : http://%s\n", cfg.deviceIP.c_str());
    Serial.printf("  Hostname     : http://%s.local\n", MDNS_NAME);
    Serial.println("  (Connect your device to the hotspot first)");
    Serial.println("========================================\n");
  } else {
    wifiMode = MODE_NONE;
    Serial.println("[AP] FAILED to start access point!");
  }
}

// ─── Auto-select: try STA first, fall back to AP ─────────────────────
// Behaviour:
//   1. Attempt STA using cfg.wifiSSID / cfg.wifiPass (loaded from flash).
//      If SSID is blank or credentials are wrong → skip straight to AP.
//   2. If STA succeeds → sync NTP time, done.
//   3. If STA fails   → launch AP hotspot so you can reach the dashboard,
//      update the credentials in Configuration panel, and reboot.
void startNetwork() {
  bool staOK = (cfg.wifiSSID.length() > 0) && startSTA();
  if (staOK) {
    syncTime();   // NTP only works in STA mode
  } else {
    startAP();
    Serial.println("[INFO] No NTP in AP mode — timestamps will show N/A.");
    Serial.println("[INFO] To get time, reconnect to a router and reboot.");
  }
}

void syncTime() {
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
  Serial.print("NTP sync");
  struct tm t;
  for (int i = 0; i < 20 && !getLocalTime(&t); i++) { delay(500); Serial.print("."); }
  Serial.println(getLocalTime(&t) ? " OK" : " FAILED");
}

// ══════════════════════════════════════════════════════════════════════
//  SPIFFS HELPERS
// ══════════════════════════════════════════════════════════════════════

int countRows() {
  File f = SPIFFS.open(CSV_PATH, "r");
  if (!f) return 0;
  int count = -1;
  while (f.available()) { if (f.readStringUntil('\n').length() > 0) count++; }
  f.close();
  return max(0, count);
}

void rotateLogs() {
  size_t freeBytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
  if (freeBytes > SPIFFS_MIN_FREE) return;
  Serial.println("[INFO] Flash low — rotating log...");
  File src = SPIFFS.open(CSV_PATH, "r");
  if (!src) return;
  int total = 0;
  while (src.available()) { src.readStringUntil('\n'); total++; }
  src.seek(0);
  int skipRows = total / 5;
  int lineNum  = 0;
  String tmp   = "";
  while (src.available()) {
    String line = src.readStringUntil('\n');
    lineNum++;
    if (lineNum == 1 || lineNum > (1 + skipRows)) tmp += line + "\n";
  }
  src.close();
  File dst = SPIFFS.open(CSV_PATH, "w");
  if (dst) { dst.print(tmp); dst.close(); }
  rowCount = countRows();
  Serial.printf("[INFO] Rotation done. Rows: %d\n", rowCount);
}

void appendCSVRow(float v, float i, float p, float e) {
  rotateLogs();
  File f = SPIFFS.open(CSV_PATH, "a");
  if (!f) { Serial.println("[ERROR] Cannot open CSV."); return; }
  char row[72];
  snprintf(row, sizeof(row), "%s,%s,%.2f,%.2f,%.1f,%.3f\n",
           getDate().c_str(), getTimeStr().c_str(), v, i, p, e);
  f.print(row);
  f.close();
  rowCount++;
}

// ══════════════════════════════════════════════════════════════════════
//  MODBUS / PZEM
// ══════════════════════════════════════════════════════════════════════

// Human-readable Modbus error descriptions
String modbusErrStr(uint8_t code) {
  switch (code) {
    case 0xE0: return "0xE0 — Illegal Function";
    case 0xE1: return "0xE1 — Illegal Data Address";
    case 0xE2: return "0xE2 — Response Timeout (check wiring / slave ID)";
    case 0xE3: return "0xE3 — Invalid Slave ID in response";
    case 0xE4: return "0xE4 — Invalid Function in response";
    case 0xE5: return "0xE5 — Response CRC mismatch";
    case 0xE6: return "0xE6 — Response too long";
    default:   return "0x" + String(code, HEX) + " — Unknown error";
  }
}

// ─── Relay 2 state manager ─────────────────────────────────────────────
// Call from loop() on every iteration. Handles three modes:
//   ERROR   → blink every BLINK_INTERVAL_MS (non-blocking, millis-based)
//   LOGGING → relay ON solid
//   IDLE    → relay OFF
// This is the ONLY place that should write to RELAY2_PIN so behaviour
// stays consistent regardless of which code path triggered a state change.
void updateRelay2() {
  unsigned long now = millis();

  if (!lastReadOK) {
    // ── Error mode: blink ─────────────────────────────────────────────
    if (now - lastBlinkTime >= BLINK_INTERVAL_MS) {
      lastBlinkTime = now;
      blinkState = !blinkState;
      // V10 convention: HIGH = ON, LOW = OFF
      digitalWrite(RELAY2_PIN, blinkState ? HIGH : LOW);
    }
  } else {
    // ── Normal mode: reflect logging state ────────────────────────────
    // Reset blink state so next error always starts with LED ON
    blinkState    = false;
    lastBlinkTime = 0;
    digitalWrite(RELAY2_PIN, loggingActive ? HIGH : LOW);
  }
}

// ─── Read PZEM-017 via Modbus RTU ─────────────────────────────────────
bool readPZEM() {
  pzem.begin(cfg.modbusID, rs485Serial);   // Apply live Modbus ID
  uint8_t result = pzem.readInputRegisters(0x0000, 8);
  if (result == pzem.ku8MBSuccess) {
    lastVoltage = pzem.getResponseBuffer(0) / 100.0f;
    lastCurrent = pzem.getResponseBuffer(1) / 100.0f;
    uint32_t rP = ((uint32_t)pzem.getResponseBuffer(3) << 16) | pzem.getResponseBuffer(2);
    uint32_t rE = ((uint32_t)pzem.getResponseBuffer(5) << 16) | pzem.getResponseBuffer(4);
    lastPower   = rP / 10.0f;
    lastEnergy  = rE / 1000.0f;
    lastReadOK  = true;
    lastError   = "";

    // Voltage threshold check — evaluated every successful read so
    // the dashboard status indicator reflects current conditions
    // immediately, independent of whether logging is active.
    highVoltAlarm = (lastVoltage >= cfg.highVoltThresh);
    lowVoltAlarm  = (lastVoltage <= cfg.lowVoltThresh);

    return true;
  }
  lastReadOK = false;
  lastError  = modbusErrStr(result);
  errorCount++;
  Serial.printf("[ERROR] Modbus %s\n", lastError.c_str());
  return false;
}

// ══════════════════════════════════════════════════════════════════════
//  WEB HANDLERS
// ══════════════════════════════════════════════════════════════════════

void handleRoot() {
  size_t used  = SPIFFS.usedBytes();
  size_t total = SPIFFS.totalBytes();
  int    pct   = (int)(100.0f * used / total);
  String barCls = pct > 90 ? "full" : pct > 70 ? "warn" : "";

  String html = R"rawhtml(<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Dyno Logger</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:'Courier New',monospace;background:#0a0a0a;color:#00ff88;padding:14px}
  h1{color:#00cfff;font-size:1.25em;letter-spacing:3px;margin-bottom:14px}
  h2{color:#00cfff;font-size:.85em;letter-spacing:2px;margin:18px 0 8px;
     text-transform:uppercase;border-bottom:1px solid #1a1a1a;padding-bottom:4px}
  .grid{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:12px}
  .card{background:#111;border:1px solid #00ff8833;border-radius:8px;
        padding:12px 16px;min-width:130px;flex:1}
  .card.err-card{border-color:#ff444466;background:#1a0808}
  .lbl{font-size:.7em;color:#555;text-transform:uppercase;letter-spacing:1px}
  .val{font-size:1.9em;font-weight:bold;line-height:1.1;margin:2px 0}
  .unit{font-size:.75em;color:#444}
  .alarm{color:#ff4444;font-weight:bold;font-size:.85em;margin:2px 0}
  .err-box{background:#1a0808;border:1px solid #ff444466;border-radius:8px;
           padding:10px 14px;margin-bottom:12px;color:#ff6666;font-size:.82em}
  .err-box .elbl{color:#ff4444;font-size:.75em;text-transform:uppercase;letter-spacing:1px}
  .actions{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:14px}
  .btn{padding:9px 16px;border-radius:7px;text-decoration:none;border:none;
       font-family:inherit;font-weight:bold;font-size:.85em;cursor:pointer;display:inline-block}
  .dl{background:#00ff88;color:#000}
  .cl{background:#ff4444;color:#fff}
  .sv{background:#00aaff;color:#000}
  .bar-wrap{background:#1a1a1a;border-radius:5px;height:8px;margin:3px 0 10px}
  .bar{height:8px;border-radius:5px}
  .bar.ok{background:#00ff88}.bar.warn{background:#ffaa00}.bar.full{background:#ff4444}
  .meta{color:#444;font-size:.76em;line-height:2}
  .ok{color:#00ff88}.er{color:#ff4444}.warn{color:#ffaa00}
  /* Config panel */
  .cfg-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}
  @media(max-width:500px){.cfg-grid{grid-template-columns:1fr}}
  .fg{display:flex;flex-direction:column;gap:3px}
  .fg label{font-size:.72em;color:#555;text-transform:uppercase;letter-spacing:.5px}
  .fg input{background:#111;border:1px solid #00ff8833;border-radius:5px;
            color:#00ff88;font-family:inherit;font-size:.85em;padding:6px 8px;width:100%}
  .fg input:focus{outline:none;border-color:#00ff88}
  .fg input[readonly]{color:#444;cursor:not-allowed}
  .note{color:#444;font-size:.72em;margin-top:6px}
  .status-dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px}
  .dot-ok{background:#00ff88}.dot-err{background:#ff4444}
  /* Voltage alarm indicator */
  .volt-status{display:flex;align-items:center;gap:8px;padding:10px 14px;
               border-radius:8px;margin-bottom:12px;font-size:.85em;font-weight:bold}
  .volt-status.normal{background:#0a1a10;border:1px solid #00ff8844;color:#00ff88}
  .volt-status.high{background:#1a0808;border:1px solid #ff444466;color:#ff4444;
                     animation:pulse-red 1.2s infinite}
  .volt-status.low{background:#1a1408;border:1px solid #ffaa0066;color:#ffaa00;
                    animation:pulse-amber 1.2s infinite}
  @keyframes pulse-red{0%,100%{opacity:1}50%{opacity:.55}}
  @keyframes pulse-amber{0%,100%{opacity:1}50%{opacity:.6}}
  .volt-dot{width:10px;height:10px;border-radius:50%;flex-shrink:0}
  .volt-dot.normal{background:#00ff88}
  .volt-dot.high{background:#ff4444}
  .volt-dot.low{background:#ffaa00}
  /* Logging session controls */
  .session-badge{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;
                 border-radius:20px;font-size:.72em;font-weight:bold;margin-left:8px}
  .session-badge.running{background:#0a1a10;color:#00ff88;border:1px solid #00ff8866}
  .session-badge.stopped{background:#1a1a1a;color:#666;border:1px solid #333}
  .pulse-dot{width:7px;height:7px;border-radius:50%;background:#00ff88;
             animation:pulse-red 1s infinite}
  .start{background:#00ff88;color:#000}
  .stop{background:#ff4444;color:#fff}
  .btn:disabled{opacity:.35;cursor:not-allowed}
  /* Trend chart */
  .chart-wrap{background:#111;border:1px solid #00ff8833;border-radius:8px;
              padding:10px;margin-bottom:12px}
  .chart-legend{display:flex;gap:14px;font-size:.75em;margin-bottom:6px;flex-wrap:wrap}
  .chart-legend span{display:flex;align-items:center;gap:5px}
  .swatch{width:10px;height:10px;border-radius:2px;display:inline-block}
  .sw-v{background:#00cfff}.sw-i{background:#ffaa00}.sw-p{background:#00ff88}
  .sw-e{background:#ff66cc}
  .chart-empty{color:#444;font-size:.8em;text-align:center;padding:30px 0}
  #trendCanvas{width:100%;height:180px;display:block}
  .chart-tabs{display:flex;gap:0;margin-bottom:10px;border-bottom:1px solid #1a1a1a}
  .ctab{background:none;border:none;border-bottom:2px solid transparent;
        color:#555;font-family:'Courier New',monospace;font-size:.78em;
        font-weight:bold;padding:6px 14px;cursor:pointer;text-transform:uppercase;
        letter-spacing:1px;transition:color .2s,border-color .2s}
  .ctab:hover{color:#00ff88}
  .ctab.active{color:#00cfff;border-bottom-color:#00cfff}
  #trendCanvas{width:100%;height:180px;display:block}
  /* Memo button */
  .memo-btn{position:fixed;top:14px;right:14px;width:34px;height:34px;border-radius:50%;
            background:#1a1a1a;border:1px solid #00ff8866;color:#00ff88;font-size:1.1em;
            font-weight:bold;cursor:pointer;display:flex;align-items:center;
            justify-content:center;z-index:900;transition:background .2s}
  .memo-btn:hover{background:#00ff8822}
  /* Memo modal overlay */
  .memo-overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.75);
                z-index:1000;align-items:center;justify-content:center}
  .memo-overlay.open{display:flex}
  .memo-modal{background:#111;border:1px solid #00ff8844;border-radius:12px;
              width:min(680px,96vw);max-height:88vh;display:flex;flex-direction:column;
              box-shadow:0 0 40px #00ff8822}
  .memo-header{display:flex;align-items:center;justify-content:space-between;
               padding:14px 18px;border-bottom:1px solid #1a1a1a}
  .memo-header h3{color:#00cfff;font-size:.95em;letter-spacing:2px;
                  text-transform:uppercase;margin:0}
  .memo-close{background:none;border:none;color:#555;font-size:1.4em;
              cursor:pointer;line-height:1;padding:0 4px}
  .memo-close:hover{color:#ff4444}
  .memo-body{flex:1;padding:14px 18px;overflow:hidden;display:flex;flex-direction:column}
  .memo-textarea{flex:1;min-height:360px;max-height:60vh;background:#0a0a0a;
                 border:1px solid #00ff8833;border-radius:7px;color:#00ff88;
                 font-family:'Courier New',monospace;font-size:.88em;line-height:1.7;
                 padding:12px 14px;resize:vertical;width:100%;outline:none;
                 overflow-y:auto;tab-size:2}
  .memo-textarea:focus{border-color:#00ff88}
  .memo-textarea::placeholder{color:#333}
  .memo-footer{display:flex;align-items:center;justify-content:space-between;
               padding:12px 18px;border-top:1px solid #1a1a1a;gap:8px;flex-wrap:wrap}
  .memo-save-btn{background:#00aaff;color:#000;border:none;border-radius:7px;
                 padding:8px 18px;font-family:inherit;font-weight:bold;font-size:.85em;
                 cursor:pointer}
  .memo-save-btn:hover{background:#00cfff}
  .memo-status{font-size:.75em;color:#444}
</style>
</head><body>
<h1>&#9889; DYNO DATA LOGGER</h1>

<!-- Memo button — fixed top-right corner -->
<button class='memo-btn' onclick='openMemo()' title='Notes &amp; Instructions'>?</button>

<!-- Memo modal -->
<div id='memo-overlay' class='memo-overlay' onclick='closeMemoOutside(event)'>
  <div class='memo-modal'>
    <div class='memo-header'>
      <h3>&#128221; Notes &amp; Instructions</h3>
      <button class='memo-close' onclick='closeMemo()' title='Close'>&times;</button>
    </div>
    <div class='memo-body'>
      <textarea id='memo-text' class='memo-textarea'
        placeholder='Type your operating instructions, troubleshooting steps, or notes here...&#10;&#10;This memo is saved to the ESP32 flash and persists across reboots.'
        spellcheck='false'></textarea>
    </div>
    <div class='memo-footer'>
      <span id='memo-status' class='memo-status'>Loading...</span>
      <button class='memo-save-btn' onclick='saveMemo()'>&#128190; Save Memo</button>
    </div>
  </div>
</div>
)rawhtml";

  // ── Live readings ──────────────────────────────────────────────────
  html += "<h2>Live Readings</h2><div class='grid' id='readings-grid'>";
  if (lastReadOK) {
    auto card = [](const String& lbl, const String& val, const String& unit) {
      return "<div class='card'><div class='lbl'>" + lbl + "</div>"
             "<div class='val'>" + val + "</div><div class='unit'>" + unit + "</div></div>";
    };
    html += card("Voltage", String(lastVoltage, 2), "V");
    html += card("Current", String(lastCurrent, 2), "A");
    html += card("Power",   String(lastPower,   1), "W");
    html += card("Energy",  String(lastEnergy,  3), "kWh");
  } else {
    html += "<div class='card err-card' style='flex:1'>";
    html += "<div class='lbl'>Status</div>";
    html += "<div class='val er' style='font-size:1em'>No Data</div></div>";
  }
  html += "</div>";

  // ── Voltage status indicator ───────────────────────────────────────
  String voltCls = "normal", voltIcon = "&#10003;", voltMsg = "Voltage normal";
  if (highVoltAlarm) { voltCls = "high"; voltIcon = "&#9888;"; voltMsg = "HIGH VOLTAGE — " + String(lastVoltage,1) + "V exceeds " + String(cfg.highVoltThresh,0) + "V threshold"; }
  else if (lowVoltAlarm) { voltCls = "low"; voltIcon = "&#9888;"; voltMsg = "LOW VOLTAGE — " + String(lastVoltage,1) + "V below " + String(cfg.lowVoltThresh,0) + "V threshold"; }
  html += "<div id='volt-status' class='volt-status " + voltCls + "'>";
  html += "<span id='volt-dot' class='volt-dot " + voltCls + "'></span>";
  html += "<span id='volt-msg'>" + voltMsg + "</span>";
  html += "</div>";
  html += "<div id='error-box' class='err-box'" + String(!lastReadOK || errorCount > 0 ? "" : " style='display:none'") + ">";
    html += "<div class='elbl'>&#9888; Modbus Error Log</div>";
    if (!lastError.isEmpty())
      html += "<div style='margin-top:4px'>Last error &nbsp;: <b>" + lastError + "</b></div>";
    html += "<div>Error count : <b>" + String(errorCount) + "</b> this session</div>";
    if (lastReadOK)
      html += "<div class='ok' style='margin-top:4px'>&#10003; Last read successful</div>";
  html += "</div>";

  // ── Trend charts — tabbed single panel ───────────────────────────
  html += "<h2>Trends</h2>";
  html += "<div class='chart-wrap'>";
  // Tab bar
  html += "<div class='chart-tabs'>";
  html += "<button class='ctab active' onclick='switchTab(this,\"ve\")'>Voltage &amp; Energy</button>";
  html += "<button class='ctab' onclick='switchTab(this,\"ip\")'>Current &amp; Power</button>";
  html += "</div>";
  // Legends (swapped in/out with the canvas)
  html += "<div id='legend-ve' class='chart-legend'>";
  html += "<span><i class='swatch sw-v'></i>Voltage (V)</span>";
  html += "<span><i class='swatch sw-e'></i>Energy (kWh)</span>";
  html += "</div>";
  html += "<div id='legend-ip' class='chart-legend' style='display:none'>";
  html += "<span><i class='swatch sw-i'></i>Current (A)</span>";
  html += "<span><i class='swatch sw-p'></i>Power (W)</span>";
  html += "</div>";
  // Single canvas — redrawn on tab switch
  html += "<canvas id='trendCanvas' width='600' height='180'></canvas>";
  html += "<div id='trend-empty' class='chart-empty' style='display:none'>Waiting for data points&hellip;</div>";
  html += "</div>";

  // ── Alarm badges ──────────────────────────────────────────────────
  // (stored from last successful read — we use globals for highAlarm/lowAlarm)

  // ── Action buttons ────────────────────────────────────────────────
  html += "<h2>Actions <span id='session-badge' class='session-badge " +
          String(loggingActive ? "running" : "stopped") + "'>" +
          (loggingActive ? "<span class='pulse-dot'></span>LOGGING" : "STOPPED") +
          "</span></h2><div class='actions'>";
  html += "<button id='btn-start' class='btn start' onclick='startLogging()'" +
          String(loggingActive ? " disabled" : "") + ">&#9654; Start Logging</button>";
  html += "<button id='btn-stop' class='btn stop' onclick='stopLogging()'" +
          String(!loggingActive ? " disabled" : "") + ">&#9632; Stop Logging</button>";
  html += "<a class='btn dl' href='/download'>&#8681; Download CSV</a>";
  html += "<a class='btn cl' href='/clear' onclick=\"return confirm('Wipe all log data?')\">&#128465; Clear Log</a>";
  html += "</div>";

  // ── Flash usage ───────────────────────────────────────────────────
  html += "<h2>Storage</h2>";
  html += "<div class='meta' id='flash-txt'>Flash used: " + String(used/1024) + " KB / " +
          String(total/1024) + " KB (" + String(pct) + "%)</div>";
  html += "<div class='bar-wrap'><div id='flash-bar' class='bar " + barCls + "' style='width:" + String(pct) + "%'></div></div>";

  // ── Status info ───────────────────────────────────────────────────
  html += "<div class='meta'>";
  html += "<span id='status-dot' class='status-dot " + String(lastReadOK ? "dot-ok" : "dot-err") + "'></span>";
  html += "Modbus &nbsp;: <span id='modbus-status'>" + String(lastReadOK ? "<span class='ok'>OK</span>" : "<span class='er'>Error</span>") + "</span><br>";
  html += "Rows logged : <span class='ok' id='meta-rows'>" + String(rowCount) + "</span><br>";
  html += "Filename &nbsp;: <span class='warn' id='meta-fname'>" + buildFilename() + "</span><br>";
  html += "Date / Time : <span id='meta-dt'>" + getDate() + " &nbsp;" + getTimeStr() + "</span><br>";
  html += "Uptime &nbsp;&nbsp;&nbsp;: <span id='meta-uptime'>" + uptimeStr() + "</span><br>";

  // WiFi mode indicator
  if (wifiMode == MODE_AP) {
    html += "WiFi Mode &nbsp;: <span style='color:#ffaa00'>&#128246; Access Point (AP)</span><br>";
    html += "AP SSID &nbsp;&nbsp;&nbsp;: <span class='warn'>" + String(AP_SSID) + "</span><br>";
    html += "AP Password : <span class='warn'>" + String(AP_PASSWORD) + "</span><br>";
    html += "IP Address &nbsp;: <a href='http://" + cfg.deviceIP + "' style='color:#00ff88'>http://" + cfg.deviceIP + "</a><br>";
    html += "Hostname &nbsp;&nbsp;: <a href='http://" + String(MDNS_NAME) + ".local' style='color:#00cfff'>http://" + String(MDNS_NAME) + ".local</a><br>";
    html += "<span style='color:#555;font-size:.72em'>Connect to hotspot <b>" + String(AP_SSID) + "</b> first, then open either link above</span><br>";
    html += "<span style='color:#555;font-size:.72em'>NTP unavailable in AP mode — timestamps may show N/A</span><br>";
  } else if (wifiMode == MODE_STA) {
    html += "WiFi Mode &nbsp;: <span class='ok'>&#128225; Station (STA)</span><br>";
    html += "WiFi SSID &nbsp;: <span class='ok'>" + cfg.wifiSSID + "</span><br>";
    html += "IP Address &nbsp;: <a href='http://" + cfg.deviceIP + "' style='color:#00ff88'>http://" + cfg.deviceIP + "</a><br>";
    html += "Hostname &nbsp;&nbsp;: <a href='http://" + String(MDNS_NAME) + ".local' style='color:#00cfff'>http://" + String(MDNS_NAME) + ".local</a><br>";
    html += "<span style='color:#555;font-size:.72em'>Both links above open this dashboard (same network required for hostname)</span><br>";
  } else {
    html += "WiFi Mode &nbsp;: <span class='er'>&#10060; No connection</span><br>";
  }
  html += "</div>";

  // ── Config panel ──────────────────────────────────────────────────
  html += "<h2>Configuration</h2>";
  html += "<form action='/config' method='POST'>";
  html += "<div class='cfg-grid'>";

  auto field = [](const String& id, const String& lbl, const String& val,
                  bool ro = false, const String& note = "") {
    String s = "<div class='fg'><label for='" + id + "'>" + lbl + "</label>";
    s += "<input id='" + id + "' name='" + id + "' value='" + val + "'";
    if (ro) s += " readonly";
    s += ">";
    if (!note.isEmpty()) s += "<span class='note'>" + note + "</span>";
    s += "</div>";
    return s;
  };

  html += field("deviceIP",   "Device IP (read-only)",  cfg.deviceIP, true, "Assigned by DHCP");
  html += field("port",       "Web Port (read-only)",   String(cfg.port), true, "Reboot required to change");
  html += field("modbusID",   "Modbus Unit ID",         String(cfg.modbusID), false, "0x01–0xF7 (default: 1)");
  html += field("logInterval","Log Interval (ms)",      String(cfg.logIntervalMs), false, "Min: 1000 ms");
  html += field("folder",     "Output Folder Label",    cfg.outputFolder, false, "Label only — used for reference");
  html += field("operator",   "Operator Name",          cfg.operatorName, false, "Used in CSV filename");
  html += field("testID",     "Test ID",                cfg.testID, false, "Used in CSV filename");
  html += field("highVolt",   "High Voltage Threshold (V)", String(cfg.highVoltThresh, 1), false, "Default: 300V");
  html += field("lowVolt",    "Low Voltage Threshold (V)",  String(cfg.lowVoltThresh, 1), false, "Default: 7V");

  // WiFi credentials — in their own full-width section with reboot warning
  html += "</div>";  // close cfg-grid
  html += "<div style='margin-top:14px;padding-top:12px;border-top:1px solid #1a1a1a'>";
  html += "<div style='color:#ffaa00;font-size:.75em;margin-bottom:8px'>&#9888; WiFi Settings — changes take effect after reboot</div>";
  html += "<div class='cfg-grid'>";
  html += "<div class='fg'><label>WiFi SSID</label>";
  html += "<input name='wifiSSID' value='" + cfg.wifiSSID + "' placeholder='Enter WiFi network name'></div>";
  html += "<div class='fg'><label>WiFi Password</label>";
  html += "<div style='display:flex;gap:6px;align-items:center'>";
  html += "<input id='wifiPassInput' name='wifiPass' type='password' value='" + cfg.wifiPass + "' placeholder='Enter WiFi password' style='flex:1'>";
  html += "<button type='button' onclick='togglePass()' style='background:#1a1a1a;border:1px solid #00ff8833;"
          "color:#555;border-radius:5px;padding:6px 8px;cursor:pointer;font-size:.8em;white-space:nowrap'>"
          "&#128065; Show</button></div>";
  html += "<span class='note'>Leave blank to keep current password</span></div>";
  html += "</div>";  // close inner cfg-grid
  html += "</div>";  // close WiFi section div

  // Reboot button — separate from Save Config so it's deliberate
  html += "<br><button class='btn sv' type='submit'>&#128190; Save Config</button>";
  html += " &nbsp;<button type='button' class='btn' style='background:#ffaa00;color:#000' "
          "onclick=\"if(confirm('Save WiFi credentials and reboot now?')) { "
          "document.querySelector('form').submit(); "
          "setTimeout(function(){ fetch('/reboot'); },600); }\">&#128260; Save &amp; Reboot</button>";
  html += "<span class='note' style='margin-left:10px'>Regular Save applies immediately. Save &amp; Reboot required for WiFi changes.</span>";
  html += "</form>";

  // Don't add the closing </form> again — skip the old one below
  // (handled by removing the duplicate further down)

  // ── Auto-refresh via JS polling ──────────────────────────────────
  // Injects the current log interval so the dashboard polls /status
  // at the same rate as the logger writes rows — no full page reload,
  // so the config form is never interrupted mid-edit.
  html += "<div class='meta' style='margin-top:14px'>";
  html += "Auto-refresh: <span id='ivlabel' class='ok'>" + String(cfg.logIntervalMs/1000.0f, 1) + " s</span>";
  html += " &nbsp;|&nbsp; Last update: <span id='lastupd' class='warn'>—</span>";
  html += "</div>";

  html += R"rawhtml(
<script>
(function(){
  // Bootstrap interval from server-rendered value; JS will re-read from /status
  var pollMs = )rawhtml";
  html += String(cfg.logIntervalMs);
  html += R"rawhtml(;
  var timer = null;

  function fmt2(n){ return n < 10 ? '0'+n : ''+n; }

  function updateReadings(d){
    // Reading cards
    var grid = document.getElementById('readings-grid');
    if(!grid) return;
    if(d.modbus_ok){
      grid.innerHTML =
        card('Voltage', d.voltage.toFixed(2), 'V') +
        card('Current', d.current.toFixed(2), 'A') +
        card('Power',   d.power.toFixed(1),   'W') +
        card('Energy',  d.energy.toFixed(3),  'kWh');
    } else {
      grid.innerHTML =
        "<div class='card err-card' style='flex:1'>" +
        "<div class='lbl'>Status</div>" +
        "<div class='val er' style='font-size:1em'>No Data</div></div>";
    }

    // Error box
    var eb = document.getElementById('error-box');
    if(d.error_count > 0 || !d.modbus_ok){
      var html = "<div class='elbl'>&#9888; Modbus Error Log</div>";
      if(d.last_error) html += "<div style='margin-top:4px'>Last error&nbsp;: <b>"+d.last_error+"</b></div>";
      html += "<div>Error count : <b>"+d.error_count+"</b> this session</div>";
      if(d.modbus_ok) html += "<div class='ok' style='margin-top:4px'>&#10003; Last read successful</div>";
      eb.innerHTML = html;
      eb.style.display = '';
    } else {
      eb.style.display = 'none';
    }

    // Status meta
    var dot = document.getElementById('status-dot');
    var mbtxt = document.getElementById('modbus-status');
    if(dot)   dot.className   = 'status-dot ' + (d.modbus_ok ? 'dot-ok' : 'dot-err');
    if(mbtxt) mbtxt.innerHTML = d.modbus_ok
      ? "<span class='ok'>OK</span>" : "<span class='er'>Error</span>";

    var el;
    if((el=document.getElementById('meta-rows')))    el.textContent = d.rows;
    if((el=document.getElementById('meta-fname')))   el.textContent = d.filename;
    if((el=document.getElementById('meta-dt')))      el.textContent = d.date+' '+d.time;
    if((el=document.getElementById('meta-uptime')))  el.textContent = d.uptime;

    // Flash bar
    var pct = Math.round(100*d.flash_used_kb/d.flash_total_kb);
    var barEl = document.getElementById('flash-bar');
    var barTxt = document.getElementById('flash-txt');
    if(barEl){
      barEl.style.width = pct+'%';
      barEl.className   = 'bar ' + (pct>90?'full':pct>70?'warn':'ok');
    }
    if(barTxt) barTxt.textContent =
      d.flash_used_kb+' KB / '+d.flash_total_kb+' KB ('+pct+'%)';

    // Interval label — re-read from JSON so it tracks config saves
    if(d.log_interval_ms && d.log_interval_ms !== pollMs){
      pollMs = d.log_interval_ms;
      reschedule();
      var ivlbl = document.getElementById('ivlabel');
      if(ivlbl) ivlbl.textContent = (pollMs/1000).toFixed(1)+' s';
    }

    // Voltage alarm indicator
    var vs = document.getElementById('volt-status');
    var vd = document.getElementById('volt-dot');
    var vm = document.getElementById('volt-msg');
    if(vs && vd && vm){
      var cls = 'normal', icon = '', msg = 'Voltage normal';
      if(d.high_volt_alarm){
        cls = 'high';
        msg = 'HIGH VOLTAGE — ' + d.voltage.toFixed(1) + 'V exceeds ' + d.high_volt_thresh.toFixed(0) + 'V threshold';
      } else if(d.low_volt_alarm){
        cls = 'low';
        msg = 'LOW VOLTAGE — ' + d.voltage.toFixed(1) + 'V below ' + d.low_volt_thresh.toFixed(0) + 'V threshold';
      }
      vs.className = 'volt-status ' + cls;
      vd.className = 'volt-dot ' + cls;
      vm.textContent = msg;
    }

    // Logging session badge + Start/Stop button states
    var badge = document.getElementById('session-badge');
    var btnStart = document.getElementById('btn-start');
    var btnStop  = document.getElementById('btn-stop');
    if(badge){
      if(d.logging_active){
        badge.className = 'session-badge running';
        badge.innerHTML = "<span class='pulse-dot'></span>LOGGING";
      } else {
        badge.className = 'session-badge stopped';
        badge.textContent = 'STOPPED';
      }
    }
    if(btnStart) btnStart.disabled = d.logging_active;
    if(btnStop)  btnStop.disabled  = !d.logging_active;

    // Timestamp
    var now = new Date();
    var ts  = fmt2(now.getHours())+':'+fmt2(now.getMinutes())+':'+fmt2(now.getSeconds());
    var upd = document.getElementById('lastupd');
    if(upd) upd.textContent = ts;
  }

  function card(lbl,val,unit){
    return "<div class='card'><div class='lbl'>"+lbl+"</div>"+
           "<div class='val'>"+val+"</div><div class='unit'>"+unit+"</div></div>";
  }

  // ── Trend chart — single canvas, tab-switched ──────────────────────
  var trendCanvas = document.getElementById('trendCanvas');
  var tctx = trendCanvas ? trendCanvas.getContext('2d') : null;
  var activeSeries = 've';      // currently shown tab
  var lastPoints   = null;      // latest data from /trend

  function resizeCanvas(){
    if(!trendCanvas) return;
    var rect = trendCanvas.getBoundingClientRect();
    trendCanvas.width  = rect.width  * (window.devicePixelRatio || 1);
    trendCanvas.height = 180       * (window.devicePixelRatio || 1);
  }

  // Tab switcher — called by inline onclick on the tab buttons
  window.switchTab = function(btn, id){
    activeSeries = id;
    document.querySelectorAll('.ctab').forEach(function(b){ b.classList.remove('active'); });
    btn.classList.add('active');
    document.getElementById('legend-ve').style.display = id==='ve' ? '' : 'none';
    document.getElementById('legend-ip').style.display = id==='ip' ? '' : 'none';
    if(lastPoints) drawTrend(lastPoints);
  };

  // Compute range: min is always 0 so the baseline is meaningful.
  // Max is the series peak + 8% padding so the line never clips the top.
  function range(key, points){
    var mx = 0;
    for(var i=0;i<points.length;i++) if(points[i][key]>mx) mx=points[i][key];
    var span = Math.max(mx, 0.001);
    return {min: 0, max: mx + span * 0.08};
  }

  function drawTrend(points){
    if(!tctx) return;
    var emptyMsg = document.getElementById('trend-empty');
    if(!points || points.length < 2){
      if(emptyMsg) emptyMsg.style.display = '';
      tctx.clearRect(0,0,trendCanvas.width,trendCanvas.height);
      return;
    }
    if(emptyMsg) emptyMsg.style.display = 'none';

    var tab = activeSeries;
    var sA = tab==='ve'
      ? {key:'v', color:'#00cfff', fillColor:'rgba(0,207,255,0.12)', decimals:1}
      : {key:'i', color:'#ffaa00', fillColor:'rgba(255,170,0,0.12)', decimals:2};
    var sB = tab==='ve'
      ? {key:'e', color:'#ff66cc', fillColor:'rgba(255,102,204,0.12)', decimals:3}
      : {key:'p', color:'#00ff88', fillColor:'rgba(0,255,136,0.12)', decimals:0};

    var W = trendCanvas.width, H = trendCanvas.height;
    var padL = 50, padR = 54, padT = 10, padB = 20;
    var plotW = W - padL - padR, plotH = H - padT - padB;
    var dpr   = window.devicePixelRatio || 1;

    tctx.clearRect(0,0,W,H);

    var tMin = points[0].t, tMax = points[points.length-1].t;
    if(tMax===tMin) tMax = tMin + 1;
    var rA = range(sA.key, points);
    var rB = range(sB.key, points);

    function xPix(t){ return padL + (t-tMin)/(tMax-tMin)*plotW; }
    function yPixA(v){ return padT + (1-(v-rA.min)/(rA.max-rA.min))*plotH; }
    function yPixB(v){ return padT + (1-(v-rB.min)/(rB.max-rB.min))*plotH; }

    // Horizontal gridlines (5 lines)
    tctx.strokeStyle = '#1e1e1e'; tctx.lineWidth = 1;
    for(var g=0;g<=4;g++){
      var gy = padT + (plotH/4)*g;
      tctx.beginPath(); tctx.moveTo(padL,gy); tctx.lineTo(W-padR,gy); tctx.stroke();
    }

    // Draw one filled-area series
    function drawSeries(s, yPix, r, axisSide){
      // Fill
      tctx.beginPath();
      tctx.moveTo(xPix(points[0].t), yPix(points[0][s.key]));
      for(var i=0;i<points.length;i++) tctx.lineTo(xPix(points[i].t), yPix(points[i][s.key]));
      tctx.lineTo(xPix(points[points.length-1].t), padT+plotH);
      tctx.lineTo(xPix(points[0].t), padT+plotH);
      tctx.closePath();
      tctx.fillStyle = s.fillColor; tctx.fill();
      // Line
      tctx.beginPath();
      tctx.strokeStyle = s.color; tctx.lineWidth = 2*dpr;
      for(var i=0;i<points.length;i++){
        var x=xPix(points[i].t), y=yPix(points[i][s.key]);
        if(i===0) tctx.moveTo(x,y); else tctx.lineTo(x,y);
      }
      tctx.stroke();
      // Y-axis labels
      tctx.fillStyle = s.color;
      tctx.font = (10*dpr)+'px monospace';
      var lx = axisSide==='left' ? padL-4 : W-padR+4;
      tctx.textAlign = axisSide==='left' ? 'right' : 'left';
      tctx.fillText(r.max.toFixed(s.decimals), lx, padT+10);
      tctx.fillText('0', lx, padT+plotH+2);   // Always show 0 at the baseline
    }

    drawSeries(sA, yPixA, rA, 'left');
    drawSeries(sB, yPixB, rB, 'right');
  }

  function pollTrend(){
    fetch('/trend')
      .then(function(r){ return r.json(); })
      .then(function(d){ lastPoints = d.points; drawTrend(d.points); })
      .catch(function(){ /* silently ignore */ });
  }

  window.addEventListener('resize', function(){ resizeCanvas(); if(lastPoints) drawTrend(lastPoints); });
  resizeCanvas();

  function poll(){
    fetch('/status')
      .then(function(r){ return r.json(); })
      .then(function(d){ updateReadings(d); })
      .catch(function(){ /* silently ignore — ESP32 busy */ });
    pollTrend();   // Feeds the single tabbed canvas
  }

  function reschedule(){
    if(timer) clearInterval(timer);
    timer = setInterval(poll, pollMs);
  }

  // ── Start/Stop logging — fetch-based, no full page reload ──────────
  // Exposed on window since the buttons use inline onclick handlers.
  // Password show/hide toggle
  window.togglePass = function(){
    var inp = document.getElementById('wifiPassInput');
    if(!inp) return;
    inp.type = inp.type === 'password' ? 'text' : 'password';
  };

  window.startLogging = function(){
    fetch('/start').then(function(){ poll(); }).catch(function(){});
  };
  window.stopLogging = function(){
    fetch('/stop').then(function(){ poll(); }).catch(function(){});
  };

  // ── Memo modal ─────────────────────────────────────────────────────
  var memoLoaded = false;

  window.openMemo = function(){
    var overlay = document.getElementById('memo-overlay');
    overlay.classList.add('open');
    // Load memo text from ESP32 on first open (lazy load)
    if(!memoLoaded){
      document.getElementById('memo-status').textContent = 'Loading...';
      fetch('/memo')
        .then(function(r){ return r.text(); })
        .then(function(txt){
          document.getElementById('memo-text').value = txt;
          memoLoaded = true;
          document.getElementById('memo-status').textContent =
            txt.length ? txt.length + ' characters' : 'Empty — start typing!';
        })
        .catch(function(){
          document.getElementById('memo-status').textContent = 'Could not load memo.';
        });
    }
    // Focus textarea so user can type immediately
    setTimeout(function(){ document.getElementById('memo-text').focus(); }, 80);
  };

  window.closeMemo = function(){
    document.getElementById('memo-overlay').classList.remove('open');
  };

  // Close if user clicks the dim overlay behind the modal
  window.closeMemoOutside = function(e){
    if(e.target === document.getElementById('memo-overlay')) window.closeMemo();
  };

  // Save memo via POST /memo — no page reload
  window.saveMemo = function(){
    var text = document.getElementById('memo-text').value;
    var statusEl = document.getElementById('memo-status');
    statusEl.textContent = 'Saving...';
    fetch('/memo', {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: 'memo=' + encodeURIComponent(text)
    })
    .then(function(r){
      if(r.ok){
        statusEl.textContent = 'Saved \u2713  (' + text.length + ' characters)';
        setTimeout(function(){ statusEl.textContent = text.length + ' characters'; }, 2500);
      } else {
        statusEl.textContent = 'Save failed.';
      }
    })
    .catch(function(){ statusEl.textContent = 'Save error.'; });
  };

  // Ctrl+S / Cmd+S shortcut saves the memo while modal is open
  document.addEventListener('keydown', function(e){
    if((e.ctrlKey||e.metaKey) && e.key==='s'){
      if(document.getElementById('memo-overlay').classList.contains('open')){
        e.preventDefault();
        window.saveMemo();
      }
    }
    // Escape closes the modal
    if(e.key==='Escape') window.closeMemo();
  });

  // Kick off immediately + schedule
  poll();
  reschedule();
})();
</script>
</body></html>)rawhtml";

  server.send(200, "text/html", html);
}

// ─── POST /config ─────────────────────────────────────────────────────
void handleConfig() {
  if (server.hasArg("modbusID")) {
    int id = server.arg("modbusID").toInt();
    if (id >= 1 && id <= 0xF7) cfg.modbusID = (uint8_t)id;
  }
  if (server.hasArg("logInterval")) {
    uint32_t iv = server.arg("logInterval").toInt();
    cfg.logIntervalMs = max((uint32_t)1000, iv);
  }
  if (server.hasArg("folder"))   cfg.outputFolder  = server.arg("folder");
  if (server.hasArg("operator")) cfg.operatorName  = server.arg("operator");
  if (server.hasArg("testID"))   cfg.testID        = server.arg("testID");
  if (server.hasArg("highVolt")) cfg.highVoltThresh = server.arg("highVolt").toFloat();
  if (server.hasArg("lowVolt"))  cfg.lowVoltThresh  = server.arg("lowVolt").toFloat();
  // WiFi credentials — only update if a non-empty SSID was provided
  if (server.hasArg("wifiSSID") && server.arg("wifiSSID").length() > 0)
    cfg.wifiSSID = server.arg("wifiSSID");
  // Password: only overwrite if user typed something; blank = keep existing
  if (server.hasArg("wifiPass") && server.arg("wifiPass").length() > 0)
    cfg.wifiPass = server.arg("wifiPass");

  saveConfig();

  // Redirect back to dashboard
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// ─── GET /reboot ──────────────────────────────────────────────────────
// Called by the "Save & Reboot" button after the form POST has already
// saved config. Sends a response first so the browser isn't left hanging,
// then waits 800ms before rebooting so the response can be transmitted.
void handleReboot() {
  server.send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{font-family:monospace;background:#0a0a0a;color:#00ff88;"
    "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
    "h2{text-align:center;color:#00cfff}</style></head><body>"
    "<h2>&#128260; Rebooting...<br><span style='font-size:.6em;color:#555'>"
    "Reconnect to WiFi if credentials changed.<br>"
    "If STA fails, connect to <b style='color:#ffaa00'>DynoLogger</b> AP.</span></h2>"
    "</body></html>");
  server.client().flush();
  delay(800);
  ESP.restart();
}

// ─── GET /download ────────────────────────────────────────────────────
void handleDownload() {
  File f = SPIFFS.open(CSV_PATH, "r");
  if (!f) { server.send(500, "text/plain", "Log file not found."); return; }
  String fname = buildFilename();
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + fname + "\"");
  server.sendHeader("Content-Length", String(f.size()));
  server.streamFile(f, "text/csv");
  f.close();
}

// ─── GET /clear ───────────────────────────────────────────────────────
void handleClear() {
  File f = SPIFFS.open(CSV_PATH, "w");
  if (f) { f.print(CSV_HEADER); f.close(); }
  rowCount   = 0;
  errorCount = 0;
  lastError  = "";
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// ─── GET /start — begin a logging session ──────────────────────────────
// Modbus polling and live readings continue regardless; this only
// gates whether readings get written to the CSV. Lets you watch the
// dashboard, position your test rig, then start the run cleanly
// without a junk row at the very beginning.
void handleStart() {
  loggingActive = true;
  // Relay 2 is managed by updateRelay2() in loop() — no digitalWrite here.
  Serial.println("[SESSION] Logging started.");
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// ─── GET /stop — pause a logging session ───────────────────────────────
void handleStop() {
  loggingActive = false;
  // Relay 2 is managed by updateRelay2() in loop() — no digitalWrite here.
  Serial.println("[SESSION] Logging stopped.");
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// ─── GET /status (JSON) ───────────────────────────────────────────────
void handleStatus() {
  size_t used = SPIFFS.usedBytes(), total = SPIFFS.totalBytes();
  String j = "{";
  j += "\"rows\":"          + String(rowCount)           + ",";
  j += "\"error_count\":"   + String(errorCount)         + ",";
  j += "\"last_error\":\""  + lastError                  + "\",";
  j += "\"modbus_ok\":"     + String(lastReadOK ? "true" : "false") + ",";
  j += "\"voltage\":"       + String(lastVoltage, 2)     + ",";
  j += "\"current\":"       + String(lastCurrent, 2)     + ",";
  j += "\"power\":"         + String(lastPower,   1)     + ",";
  j += "\"energy\":"        + String(lastEnergy,  3)     + ",";
  j += "\"high_volt_alarm\":" + String(highVoltAlarm ? "true" : "false") + ",";
  j += "\"low_volt_alarm\":"  + String(lowVoltAlarm  ? "true" : "false") + ",";
  j += "\"high_volt_thresh\":"+ String(cfg.highVoltThresh, 1) + ",";
  j += "\"low_volt_thresh\":" + String(cfg.lowVoltThresh, 1)  + ",";
  j += "\"logging_active\":"  + String(loggingActive ? "true" : "false") + ",";
  j += "\"flash_used_kb\":" + String(used/1024)          + ",";
  j += "\"flash_total_kb\":"+ String(total/1024)         + ",";
  j += "\"modbus_id\":"     + String(cfg.modbusID)       + ",";
  j += "\"log_interval_ms\":"+ String(cfg.logIntervalMs) + ",";
  j += "\"log_interval\":"  + String(cfg.logIntervalMs)  + ",";
  j += "\"operator\":\""    + cfg.operatorName           + "\",";
  j += "\"test_id\":\""     + cfg.testID                 + "\",";
  j += "\"filename\":\""    + buildFilename()            + "\",";
  j += "\"uptime\":\""      + uptimeStr()                + "\",";
  j += "\"date\":\""        + getDate()                  + "\",";
  j += "\"time\":\""        + getTimeStr()               + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

// ─── GET /trend (JSON) ─────────────────────────────────────────────────
// Returns recent Voltage/Current/Power history for the dashboard chart.
// Points are returned oldest-first, reading out of the ring buffer
// starting from the oldest valid slot.
void handleTrend() {
  String j = "{\"points\":[";
  int startIdx = (trendCount < TREND_BUF_SIZE)
                   ? 0
                   : trendHead;  // oldest slot when buffer has wrapped
  for (int n = 0; n < trendCount; n++) {
    int idx = (startIdx + n) % TREND_BUF_SIZE;
    if (n > 0) j += ",";
    j += "{\"t\":" + String(trendBuf[idx].t) +
         ",\"v\":" + String(trendBuf[idx].v, 2) +
         ",\"i\":" + String(trendBuf[idx].i, 2) +
         ",\"p\":" + String(trendBuf[idx].p, 1) +
         ",\"e\":" + String(trendBuf[idx].e, 3) + "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

// ─── GET /memo — read memo text from SPIFFS ───────────────────────────
// Returns raw plain text; JS inserts it into the textarea directly.
// Returns empty string if no memo file exists yet (first-time use).
void handleMemoGet() {
  if (!SPIFFS.exists(MEMO_PATH)) {
    server.send(200, "text/plain; charset=utf-8", "");
    return;
  }
  File f = SPIFFS.open(MEMO_PATH, "r");
  if (!f) { server.send(500, "text/plain", "Cannot open memo."); return; }
  server.streamFile(f, "text/plain; charset=utf-8");
  f.close();
}

// ─── POST /memo — write memo text to SPIFFS ────────────────────────────
// Expects application/x-www-form-urlencoded body: memo=<encoded text>
// The WebServer library decodes the URL encoding automatically.
// Max practical memo size is ~10 KB — more than enough for instructions.
void handleMemoPost() {
  if (!server.hasArg("memo")) {
    server.send(400, "text/plain", "Missing memo field.");
    return;
  }
  String text = server.arg("memo");
  File f = SPIFFS.open(MEMO_PATH, "w");
  if (!f) { server.send(500, "text/plain", "Cannot write memo."); return; }
  f.print(text);
  f.close();
  Serial.printf("[MEMO] Saved %d bytes.\n", text.length());
  server.send(200, "text/plain", "OK");
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ══════════════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════════════

void setup() {
  bootTime = millis();
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== KC868-A2 + PZEM-017 | DYNO LOGGER ===");

  // ── Relay init ─────────────────────────────────────────────────────
  // KC868-A2 relays are active-LOW: LOW = energised (ON), HIGH = off
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, HIGH);    // Relay 1 ON immediately — unit is powered and running
  digitalWrite(RELAY2_PIN, LOW);   // Relay 2 OFF — logging not started yet
  Serial.println("Relay 1 ON (power), Relay 2 OFF (logging).");

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("[ERROR] SPIFFS mount failed!");
  } else {
    Serial.printf("SPIFFS OK — %u KB / %u KB\n",
                  SPIFFS.usedBytes()/1024, SPIFFS.totalBytes()/1024);
    loadConfig();
    if (!SPIFFS.exists(CSV_PATH)) {
      File f = SPIFFS.open(CSV_PATH, "w");
      if (f) { f.print(CSV_HEADER); f.close(); }
    }
    rowCount = countRows();
    Serial.printf("Existing rows: %d\n", rowCount);
  }

  // Modbus
  rs485Serial.begin(BAUD_RATE, SERIAL_8N2, RS485_RX_PIN, RS485_TX_PIN);
  pzem.begin(cfg.modbusID, rs485Serial);
  pzem.preTransmission(preTransmission);
  pzem.postTransmission(postTransmission);
  Serial.println("Modbus initialised.");

  // WiFi (STA → AP fallback) + NTP
  startNetwork();

  // Web server
  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/download", HTTP_GET,  handleDownload);
  server.on("/clear",    HTTP_GET,  handleClear);
  server.on("/start",    HTTP_GET,  handleStart);
  server.on("/stop",     HTTP_GET,  handleStop);
  server.on("/config",   HTTP_POST, handleConfig);
  server.on("/status",   HTTP_GET,  handleStatus);
  server.on("/trend",    HTTP_GET,  handleTrend);
  server.on("/reboot",   HTTP_GET,  handleReboot);
  server.on("/memo",     HTTP_GET,  handleMemoGet);
  server.on("/memo",     HTTP_POST, handleMemoPost);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started.");

  delay(2000); // PZEM boot time
}

// ══════════════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();
  updateRelay2();   // Non-blocking relay 2 state machine — blink on error, normal otherwise

  unsigned long now = millis();
  if (now - lastLogTime >= cfg.logIntervalMs) {
    lastLogTime = now;

    if (readPZEM()) {
      // Trend chart and live readings always update, regardless of
      // logging state, so the dashboard stays "live" even while paused.
      pushTrendPoint(lastVoltage, lastCurrent, lastPower, lastEnergy);

      // CSV rows are only written while a session is active —
      // controlled by the Start/Stop buttons on the dashboard.
      if (loggingActive) {
        appendCSVRow(lastVoltage, lastCurrent, lastPower, lastEnergy);
      }

      Serial.printf("[%s %s] V=%.2fV I=%.2fA P=%.1fW E=%.3fkWh rows=%d %s\n",
                    getDate().c_str(), getTimeStr().c_str(),
                    lastVoltage, lastCurrent, lastPower, lastEnergy, rowCount,
                    loggingActive ? "[LOGGING]" : "[paused]");
    }
  }
}
