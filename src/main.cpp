/*
  ESP32-S3 CNC Lathe Control — web dashboard, spindle control, DRO, recorder, OTA.

  Hardware (see README for the wiring):
    Spindle encoder A/B/Z  (E6B2-CWZ6C, tapped from the G540 via 2N3904 inverters)
    X and Z step/dir       (tapped from the G540 DB25 pins 2/3 and 6/7 via inverters) -> live DRO
    Charge pump            (G540 pin 16 tap) -> "CNC Online"
    Speed knob             10k pot -> ADC
    Spindle PWM            -> 330R -> PC817 -> lathe controller pot terminals (isolated)
    2.8" ILI9341 SPI TFT   DRO, RPM, tool and status at the machine

  The ESP32 controls the spindle speed (closed loop on the encoder) and observes the axes.
  It never drives the steppers: UCCNC does that. Recording captures what UCCNC did and can
  export it as G-code; playback replays the spindle profile and shows the motion.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <vector>
#include "driver/pcnt.h"

#ifndef FW_VERSION
#define FW_VERSION "1.0.0"
#endif

// ---------------- pins (ESP32-S3-DevKitC-1) ----------------
const int PIN_ENC_A  = 4,  PIN_ENC_B = 5,  PIN_ENC_Z = 6;
const int PIN_XA     = 7,  PIN_XB = 15, PIN_ZA = 16, PIN_ZB = 17;   // X and Z axis encoders, A/B quadrature
const int PIN_CP     = 18;             // charge pump tap
const int PIN_POT    = 1;              // ADC1
const int PIN_PWM    = 2;              // -> PC817
// 2.8" ILI9341 display on the S3's native SPI2 pins (GPIO 10-12). MISO is not needed.
const int PIN_TFT_SCK = 12, PIN_TFT_MOSI = 11, PIN_TFT_CS = 10, PIN_TFT_DC = 9, PIN_TFT_RST = 8;
const int PIN_TOUCH_MISO = 13, PIN_TOUCH_CS = 14;   // the XPT2046 touch controller shares SCK and MOSI with the panel

// ---------------- settings (persisted) ----------------
struct Settings {
  char  ssid[33] = "Nuclear Reactor";
  char  pass[65] = "00110011001100110011001100";
  char  host[33] = "esp32-lathe";
  float ppr = 100;                     // encoder lines / rev
  float xPpr = 600, zPpr = 600;        // axis encoder pulses per revolution
  float xLead = 0.1f, zLead = 0.1f;    // slide travel per encoder revolution (in); X is radial travel
  bool  xDia = true;                   // show X as diameter (twice the cross-slide travel)
  bool  xInv = false, zInv = false;    // flip DRO direction
  int   maxRpm = 2500, minRpm = 60;
  float kp = 0.0004f, ki = 0.0012f;
  float xMin = -2.0f, xMax = 6.0f, zMin = -14.0f, zMax = 1.0f;   // soft limits (in)
  bool  knobEnabled = true;            // physical knob sets target; web can override
  char  manifest[128] = "";            // optional URL of {"version":"1.3.9","url":"http://.../firmware.bin"}
  int   ledRgb = 48, ledBlue = 44;     // status LED GPIOs, -1 = off (RGB is 38 on DevKitC-1 v1.1; blue is the RX LED on clone boards)
  bool  blueLow = true;                // blue LED lights when its pin is LOW
  bool  dispFlip = false;              // turn the display picture upside down, for mounting
  bool  dispSwapRb = false;            // swap red and blue: some panels wire the colours the other way round
  uint8_t dispView = 0;                // what the screen shows: 0 everything, 1 RPM only, 2 X only, 3 Z only, 4 X and Z
  int16_t tcal[6] = {0, 0, 0, 0, 0, 0}; // touch calibration: raw readings at three screen points; all zero = not calibrated
} S;
Preferences prefs;

void loadSettings() {
  prefs.begin("lathe", true);
  prefs.getString("ssid", S.ssid, sizeof S.ssid);
  prefs.getString("pass", S.pass, sizeof S.pass);
  prefs.getString("host", S.host, sizeof S.host);
  S.ppr = prefs.getFloat("ppr", S.ppr);
  S.xPpr = prefs.getFloat("xppr", S.xPpr);    S.zPpr = prefs.getFloat("zppr", S.zPpr);
  S.xLead = prefs.getFloat("xlead", S.xLead); S.zLead = prefs.getFloat("zlead", S.zLead);
  S.xDia = prefs.getBool("xdia", S.xDia);
  S.xInv = prefs.getBool("xi", false);        S.zInv = prefs.getBool("zi", false);
  S.maxRpm = prefs.getInt("maxr", S.maxRpm);  S.minRpm = prefs.getInt("minr", S.minRpm);
  S.kp = prefs.getFloat("kp", S.kp);          S.ki = prefs.getFloat("ki", S.ki);
  S.xMin = prefs.getFloat("xmin", S.xMin);    S.xMax = prefs.getFloat("xmax", S.xMax);
  S.zMin = prefs.getFloat("zmin", S.zMin);    S.zMax = prefs.getFloat("zmax", S.zMax);
  S.knobEnabled = prefs.getBool("knob", true);
  prefs.getString("mani", S.manifest, sizeof S.manifest);
  S.ledRgb = prefs.getInt("lrgb", S.ledRgb);  S.ledBlue = prefs.getInt("lblue", S.ledBlue);
  S.blueLow = prefs.getBool("blow", S.blueLow);
  S.dispFlip = prefs.getBool("dflip", S.dispFlip);
  S.dispSwapRb = prefs.getBool("dswap", S.dispSwapRb);
  S.dispView = prefs.getUChar("dview", S.dispView); if (S.dispView > 4) S.dispView = 0;
  prefs.getBytes("tcal", S.tcal, sizeof S.tcal);
  prefs.end();
}
void saveSettings() {
  prefs.begin("lathe", false);
  prefs.putString("ssid", S.ssid); prefs.putString("pass", S.pass); prefs.putString("host", S.host);
  prefs.putFloat("ppr", S.ppr);
  prefs.putFloat("xppr", S.xPpr); prefs.putFloat("zppr", S.zPpr); prefs.putFloat("xlead", S.xLead); prefs.putFloat("zlead", S.zLead);
  prefs.putBool("xdia", S.xDia);
  prefs.putBool("xi", S.xInv); prefs.putBool("zi", S.zInv);
  prefs.putInt("maxr", S.maxRpm); prefs.putInt("minr", S.minRpm);
  prefs.putFloat("kp", S.kp); prefs.putFloat("ki", S.ki);
  prefs.putFloat("xmin", S.xMin); prefs.putFloat("xmax", S.xMax); prefs.putFloat("zmin", S.zMin); prefs.putFloat("zmax", S.zMax);
  prefs.putBool("knob", S.knobEnabled); prefs.putString("mani", S.manifest);
  prefs.putInt("lrgb", S.ledRgb); prefs.putInt("lblue", S.ledBlue); prefs.putBool("blow", S.blueLow); prefs.putBool("dflip", S.dispFlip);
  prefs.putBool("dswap", S.dispSwapRb); prefs.putUChar("dview", S.dispView); prefs.putBytes("tcal", S.tcal, sizeof S.tcal);
  prefs.end();
}

// ---------------- live state ----------------
volatile uint32_t encCount = 0, encFirstUs = 0, encLastUs = 0;
volatile int32_t  encDir = 0;
// Axis encoders use the S3's hardware pulse counters in x4 quadrature mode, so no counts are lost at speed.
// The 16-bit counter returns to 0 when it reaches +/-AX_LIM, so it counts modulo AX_LIM. axisTask() reads it
// every millisecond and adds the change to a 64-bit total; that works while fewer than AX_LIM/2 counts pass
// between reads (15000 counts in 1 ms is far beyond any slide speed). Its own task keeps it running while
// loop() is blocked, for example during the Wi-Fi connect at boot.
const int16_t AX_LIM = 30000;
const uint16_t AX_FILTER = 500;                    // ignore pulses shorter than 500 APB cycles (6.25 us) as noise
int64_t axCount[2] = {0, 0}, axZero[2] = {0, 0};   // guarded by axMux
int16_t axLast[2] = {0, 0};
volatile bool zeroReq[2] = {false, false};         // set from the web task, handled in axisTask()
portMUX_TYPE axMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t cpLastUs = 0;

void IRAM_ATTR isrEncA() {
  uint32_t now = micros();
  if (encCount == 0) encFirstUs = now;
  encLastUs = now; encCount++;
  encDir += digitalRead(PIN_ENC_B) ? 1 : -1;
}
// Standard x4 decoding (as in the ESP-IDF rotary encoder example): each channel counts both edges of one
// signal, with the other signal setting the direction.
void axisSetup(pcnt_unit_t u, int a, int b) {
  pcnt_config_t c = {};
  c.unit = u; c.counter_h_lim = AX_LIM; c.counter_l_lim = -AX_LIM;
  c.channel = PCNT_CHANNEL_0; c.pulse_gpio_num = a; c.ctrl_gpio_num = b;
  c.pos_mode = PCNT_COUNT_DEC; c.neg_mode = PCNT_COUNT_INC; c.lctrl_mode = PCNT_MODE_KEEP; c.hctrl_mode = PCNT_MODE_REVERSE;
  pcnt_unit_config(&c);
  c.channel = PCNT_CHANNEL_1; c.pulse_gpio_num = b; c.ctrl_gpio_num = a;
  c.pos_mode = PCNT_COUNT_INC; c.neg_mode = PCNT_COUNT_DEC;
  pcnt_unit_config(&c);
  pcnt_set_filter_value(u, AX_FILTER); pcnt_filter_enable(u);
  pcnt_counter_pause(u); pcnt_counter_clear(u); pcnt_counter_resume(u);
}
void axisTask(void *) {
  for (;;) {
    for (int i = 0; i < 2; i++) {
      int16_t v = 0; pcnt_get_counter_value(i ? PCNT_UNIT_1 : PCNT_UNIT_0, &v);
      int32_t d = (int32_t)v - axLast[i];
      if (d > AX_LIM / 2) d -= AX_LIM; else if (d < -AX_LIM / 2) d += AX_LIM;   // the counter passed a limit and restarted at 0
      axLast[i] = v;
      portENTER_CRITICAL(&axMux);
      axCount[i] += d;
      if (zeroReq[i]) { axZero[i] = axCount[i]; zeroReq[i] = false; }
      portEXIT_CRITICAL(&axMux);
    }
    vTaskDelay(1);
  }
}
// Position in counts from zero, for both axes.
void axisRead(int64_t &x, int64_t &z) {
  portENTER_CRITICAL(&axMux); x = axCount[0] - axZero[0]; z = axCount[1] - axZero[1]; portEXIT_CRITICAL(&axMux);
}
// Counts per displayed inch: x4 decoding, and X doubled when shown as diameter.
float axCountsPerIn(int i) {
  float ppr = i ? S.zPpr : S.xPpr, lead = i ? S.zLead : S.xLead;
  return 4.0f * ppr / lead / (i == 0 && S.xDia ? 2.0f : 1.0f);
}
void IRAM_ATTR isrCp()    { cpLastUs = micros(); }

float rpm = 0, rpmShown = 0, duty = 0, integ = 0;
bool  encOk = false, spindleFwd = true, cncOnline = false;
int   targetRpm = 0, webTarget = -1;      // webTarget >= 0 overrides the knob
float xPos = 0, zPos = 0;
uint32_t spindleSeconds = 0, powerSeconds = 0;     // run hours
uint32_t lastSpindleTick = 0, lastHourSave = 0;
String jobNotes;

// ---------------- tools (kept in NVS so a filesystem upload does not erase them) ----------------
const size_t TOOL_JSON_MAX = 3800;                // NVS strings are limited to about 4000 bytes
String toolLib = "[]", toolId = "", toolName = "";

// ---------------- events & alarms ----------------
struct Event { uint32_t t; String type; String msg; };
std::vector<Event> events;                        // last 50
std::vector<Event> alarms;                        // active
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
void broadcast(const String &s) { if (ws.count()) ws.textAll(s); }

String hhmmss(uint32_t ms) {
  uint32_t s = ms / 1000; char b[16];
  snprintf(b, sizeof b, "%02u:%02u:%02u", (s / 3600) % 24, (s / 60) % 60, s % 60);
  return b;
}
void logEvent(const String &type, const String &msg) {
  events.insert(events.begin(), {millis(), type, msg});
  if (events.size() > 50) events.pop_back();
  JsonDocument d; d["ev"]["t"] = hhmmss(millis()); d["ev"]["type"] = type; d["ev"]["msg"] = msg;
  String out; serializeJson(d, out); broadcast(out);
}
void setAlarm(const String &key, const String &msg, bool on) {
  for (size_t i = 0; i < alarms.size(); i++) if (alarms[i].type == key) {
    if (!on) { alarms.erase(alarms.begin() + i); logEvent("ok", msg + " cleared"); }
    return;
  }
  if (on) { alarms.insert(alarms.begin(), {millis(), key, msg}); logEvent("warn", msg); }
}

// ---------------- recorder ----------------
struct Sample { uint32_t t; float x, z, rpm, duty; };
std::vector<Sample> rec;
bool recording = false, playing = false;
uint32_t recStart = 0, playStart = 0; size_t playIdx = 0; float playSpeed = 1;
String recName = "";

void recSave(const String &name) {
  LittleFS.mkdir("/rec");
  File f = LittleFS.open("/rec/" + name + ".csv", "w");
  if (!f) return;
  f.println("t_ms,x_in,z_in,rpm,duty");
  for (auto &s : rec) f.printf("%u,%.4f,%.4f,%.1f,%.3f\n", s.t, s.x, s.z, s.rpm, s.duty);
  f.close();
  logEvent("save", "Recording saved (" + name + ".csv)");
}
bool recLoad(const String &name) {
  File f = LittleFS.open("/rec/" + name + ".csv", "r");
  if (!f) return false;
  rec.clear(); f.readStringUntil('\n');
  while (f.available()) {
    Sample s; String l = f.readStringUntil('\n');
    if (sscanf(l.c_str(), "%u,%f,%f,%f,%f", &s.t, &s.x, &s.z, &s.rpm, &s.duty) == 5) rec.push_back(s);
  }
  f.close(); recName = name; return true;
}
// Export the recorded motion as G-code UCCNC can run (inch, diameter X, G1 with feed from the samples)
String recToGcode() {
  String g = "(Recorded on ESP32-Lathe: " + recName + ")\nG90 G18 G94\n";
  if (rec.empty()) return g + "M30\n";
  float lx = rec[0].x, lz = rec[0].z; uint32_t lt = rec[0].t; int ls = -1;
  g += "G0 X" + String(lx, 4) + " Z" + String(lz, 4) + "\n";
  for (size_t i = 1; i < rec.size(); i++) {
    const Sample &s = rec[i];
    float dx = fabsf(s.x - lx) / 2, dz = fabsf(s.z - lz);   // X is diameter: slide moves half
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist < 0.0005f) continue;
    float minutes = (s.t - lt) / 60000.0f; if (minutes <= 0) minutes = 0.001f;
    int sr = (int)(s.rpm / 10) * 10;
    if (sr != ls) { g += "S" + String(sr) + "\n"; ls = sr; }
    g += "G1 X" + String(s.x, 4) + " Z" + String(s.z, 4) + " F" + String(dist / minutes, 2) + "\n";
    lx = s.x; lz = s.z; lt = s.t;
  }
  return g + "M30\n";
}

// ---------------- threading program generator (inch, G33) ----------------
String threadProgram(float pitchMm, float majorMm, float lengthIn, bool metricOut) {
  const float tan295 = 0.5658f;
  float H = 0.61343f * pitchMm;                                // thread depth (ISO) in mm
  float depths[11]; for (int i = 0; i < 11; i++) depths[i] = H * sqrtf((i + 1) / 11.0f);
  float K = pitchMm / 25.4f, startZ = 0.2f, clearX = (majorMm + 2) / 25.4f;
  String g = "(" + String(majorMm, 2) + " x " + String(pitchMm, 3) + " mm external thread, " + String(lengthIn, 3) + " in long)\n";
  g += "(INCH UCCNC profile - X in diameter - Z0 = end face - blank " + String((majorMm - 0.05f) / 25.4f, 4) + " in)\n";
  g += "G90 G18 G40\nM3 S200\nG4 P2\nG0 X" + String(clearX, 4) + " Z" + String(startZ, 3) + "\n";
  auto pass = [&](const char *lbl, float d) {
    g += "(" + String(lbl) + " depth " + String(d / 25.4f, 4) + ")\n";
    g += "G0 Z" + String(startZ - d * tan295 / 25.4f, 4) + "\nG0 X" + String((majorMm - 2 * d) / 25.4f, 4) + "\n";
    g += "G33 Z-" + String(lengthIn, 3) + " K" + String(K, 6) + " Q0\nG0 X" + String(clearX, 4) + "\n";
  };
  for (int i = 0; i < 8; i++) pass((String("PASS ") + String(i + 1)).c_str(), depths[i]);
  g += "(FIT CHECK)\nG0 Z0.4\nM5\nM0\nM3 S200\nG4 P2\nG0 Z" + String(startZ, 3) + "\n";
  for (int i = 8; i < 11; i++) pass((String("PASS ") + String(i + 1)).c_str(), depths[i]);
  pass("SPRING 1", depths[10]); pass("SPRING 2", depths[10]);
  g += "G0 Z0.4\nM5\nM30\n";
  (void)metricOut;
  return g;
}

// ---------------- status LEDs ----------------
// RGB: green breathing when idle, blue flashing while reading the encoder or step lines, recording or playing back.
// Blue: solid on Wi-Fi, slow flash while looking for it, very fast flash once it has not been found.
// Runs as its own task so the patterns keep going while setupWifi() blocks the main loop.
const uint32_t LED_READ_HOLD_MS = 500, LED_BREATH_MS = 3000, LED_READ_FLASH_MS = 125, LED_TEST_MS = 6000;
const uint32_t WIFI_LOOK_FLASH_MS = 500, WIFI_LOST_FLASH_MS = 60, WIFI_SEARCH_MS = 15000;
const uint8_t  LED_RGB_MAX = 48;                  // full brightness is uncomfortable to look at
volatile uint32_t ledActivityMs = 0, ledTestUntil = 0;
volatile bool ledActive = false;

// Only pins this firmware does not use, and that are not strapping, USB or flash/PSRAM pins.
bool ledPinOk(int p, bool rgb) {
  if (p == -1) return true;
  if (rgb) return p == 48 || p == 38;
  for (int a : {21, 38, 39, 40, 41, 42, 43, 44, 47, 48}) if (p == a) return true;   // 8-14 are the display and its touch panel
  return false;
}
void ledTask(void *) {
  const int rgbPin = S.ledRgb;                    // neopixelWrite binds to its first pin, so a change needs a reboot
  int bluePin = -1; uint32_t lastUp = millis(); int lastR = -1, lastG = -1, lastB = -1;
  for (;;) {
    uint32_t now = millis();
    bool up = WiFi.status() == WL_CONNECTED, test = (int32_t)(ledTestUntil - now) > 0;
    if (up) lastUp = now;

    if (bluePin != S.ledBlue) {                   // the blue pin can change without a reboot
      if (bluePin >= 0) pinMode(bluePin, INPUT);
      bluePin = S.ledBlue;
      if (bluePin >= 0) pinMode(bluePin, OUTPUT);
    }
    if (bluePin >= 0) {
      bool notFound = !strlen(S.ssid) || now - lastUp >= WIFI_SEARCH_MS;
      bool on = test ? (now / 100) % 2 : up ? true : (now / (notFound ? WIFI_LOST_FLASH_MS : WIFI_LOOK_FLASH_MS)) % 2;
      digitalWrite(bluePin, on != S.blueLow ? HIGH : LOW);
    }

    if (rgbPin >= 0) {
      int r = 0, g = 0, b = 0;
      if (test) {                                 // red, green, blue in turn, to prove the wiring and colour order
        int c = (now / 500) % 3; r = c == 0 ? LED_RGB_MAX : 0; g = c == 1 ? LED_RGB_MAX : 0; b = c == 2 ? LED_RGB_MAX : 0;
      } else if (ledActive || (ledActivityMs && now - ledActivityMs < LED_READ_HOLD_MS)) {
        b = (now / LED_READ_FLASH_MS) % 2 ? LED_RGB_MAX : 0;
      } else {
        float s = 0.5f - 0.5f * cosf(TWO_PI * (now % LED_BREATH_MS) / LED_BREATH_MS);
        g = (int)(s * s * LED_RGB_MAX + 0.5f);    // squaring makes the fade look even to the eye
      }
      if (r != lastR || g != lastG || b != lastB) { neopixelWrite(rgbPin, r, g, b); lastR = r; lastG = g; lastB = b; }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------------- spindle control ----------------
const int PWM_CH = 0, PWM_BITS = 12, PWM_MAX = 4095;
// ---------------- machine display ----------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 panel; lgfx::Bus_SPI bus; lgfx::Touch_XPT2046 touch;
public:
  LGFX() {
    auto b = bus.config();
    // 20 MHz: the panel's init commands go through reliably over jumper wires. 40 MHz needs short, tidy wiring,
    // and when it fails the panel just stays asleep with a black screen. A frame takes 60 ms at 20 MHz.
    b.spi_host = SPI2_HOST; b.spi_mode = 0; b.freq_write = 20000000; b.freq_read = 16000000;
    b.spi_3wire = false; b.use_lock = true; b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = PIN_TFT_SCK; b.pin_mosi = PIN_TFT_MOSI; b.pin_miso = PIN_TOUCH_MISO; b.pin_dc = PIN_TFT_DC;
    bus.config(b); panel.setBus(&bus);
    auto p = panel.config();
    p.pin_cs = PIN_TFT_CS; p.pin_rst = PIN_TFT_RST; p.pin_busy = -1;
    p.panel_width = 240; p.panel_height = 320; p.readable = false; p.bus_shared = true;
    panel.config(p); setPanel(&panel);
    // Touch controller on the same bus, read raw at 1 MHz. The firmware does its own calibration (calibrateTouch),
    // so the library's min/max are left at the full 12-bit range.
    auto t = touch.config();
    t.spi_host = SPI2_HOST; t.freq = 1000000; t.pin_sclk = PIN_TFT_SCK; t.pin_mosi = PIN_TFT_MOSI; t.pin_miso = PIN_TOUCH_MISO;
    t.pin_cs = PIN_TOUCH_CS; t.pin_int = -1; t.bus_shared = true; t.offset_rotation = 0;
    t.x_min = 0; t.x_max = 4095; t.y_min = 0; t.y_max = 4095;
    touch.config(t); panel.setTouch(&touch);
  }
  // Some panels have red and blue the other way round. The colour-order bit lives in the rotation command,
  // so rewriting the rotation applies it.
  void setColorSwap(bool swap) { auto p = panel.config(); p.rgb_order = swap; panel.config(p); setRotation(getRotation()); }
};
LGFX lcd;
LGFX_Sprite frame(&lcd);

// What the display shows. loop() fills it and the display task copies it, both under dispLock, so the display
// never reads a String while the web task is changing it.
struct DispSnap {
  float x, z, rpm; int target, maxRpm; bool fwd, enc, cnc, rec, play, up, ap, xDia;
  char ip[16], tool[48], alarm[64]; int alarms;
  char host[33]; uint32_t spinS, powS; int rssi;      // for the Info screen
};
DispSnap dispShared = {};
SemaphoreHandle_t dispLock;

void dispSnapshot() {
  DispSnap s = {};
  s.x = xPos; s.z = zPos; s.rpm = rpmShown; s.target = targetRpm; s.maxRpm = S.maxRpm; s.fwd = spindleFwd; s.enc = encOk;
  s.cnc = cncOnline; s.rec = recording; s.play = playing; s.up = WiFi.status() == WL_CONNECTED; s.ap = WiFi.getMode() & WIFI_AP; s.xDia = S.xDia;
  strlcpy(s.ip, (s.up ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str(), sizeof s.ip);
  s.alarms = alarms.size(); if (s.alarms) strlcpy(s.alarm, alarms[0].msg.c_str(), sizeof s.alarm);
  strlcpy(s.host, S.host, sizeof s.host); s.spinS = spindleSeconds; s.powS = powerSeconds; s.rssi = s.up ? WiFi.RSSI() : 0;
  xSemaphoreTake(dispLock, portMAX_DELAY);
  strlcpy(s.tool, toolName.c_str(), sizeof s.tool);
  dispShared = s;
  xSemaphoreGive(dispLock);
}

const uint32_t DISP_FRAME_MS = 200, TOUCH_POLL_MS = 40, MENU_TIMEOUT_MS = 30000;
// Three rows of 80 px fill the screen. Numbers use the 48 px seven-segment font, scaled up to 1.5x (72 px)
// and only shrunk when a long value, such as -12.3456, would not fit the width.
const int DISP_ROW_H = 80, DISP_LABEL_W = 34;
const float DISP_MAX_SCALE = 1.5f;
// Where a frame is drawn: the off-screen frame buffer when there is memory for it, otherwise the panel itself
// (which flickers a little, but still shows the numbers).
lgfx::LovyanGFX *g = &frame;
volatile bool dispSaveReq = false;     // the on-screen menu changed a setting: loop() writes it to flash

// Smooth digits. The built-in seven-segment font scaled up 1.5x looked blocky, so the numbers now use
// anti-aliased 0-9 . - glyphs from Oswald Bold (SIL Open Font License, see fonts/OFL.txt) in three sizes:
// 120, 70 and 57 px digits. tools/make_vlw.py makes the fonts/*.vlw files from the TTF; platformio.ini's
// board_build.embed_files links them into the firmware, and they are read straight from flash.
extern const uint8_t vlw_big_start[] asm("_binary_fonts_d_big_vlw_start"), vlw_big_end[] asm("_binary_fonts_d_big_vlw_end");
extern const uint8_t vlw_mid_start[] asm("_binary_fonts_d_mid_vlw_start"), vlw_mid_end[] asm("_binary_fonts_d_mid_vlw_end");
extern const uint8_t vlw_small_start[] asm("_binary_fonts_d_small_vlw_start"), vlw_small_end[] asm("_binary_fonts_d_small_vlw_end");
lgfx::VLWfont digitFont[3];
lgfx::PointerWrapper digitData[3];
bool digitFontsOk = false;
void loadDigitFonts() {
  const uint8_t *s[3] = {vlw_big_start, vlw_mid_start, vlw_small_start}, *e[3] = {vlw_big_end, vlw_mid_end, vlw_small_end};
  digitFontsOk = true;
  for (int i = 0; i < 3; i++) { digitData[i].set(s[i], e[i] - s[i]); if (!digitFont[i].loadFont(&digitData[i])) digitFontsOk = false; }
  Serial.printf("display: digit fonts %s\n", digitFontsOk ? "loaded" : "failed, using the bitmap font");
}
// Draws a number as large as fits: the biggest smooth font no taller than maxH whose width fits maxW. The
// background colour is needed for the anti-aliased edges. Falls back to the scaled bitmap font if the smooth
// fonts did not load.
void drawNumber(const char *txt, int x, int y, int maxW, int maxH, textdatum_t datum, uint32_t col, uint32_t bg) {
  g->setTextDatum(datum);
  if (digitFontsOk) {
    int pick = 2;
    for (int i = 0; i < 3; i++) { g->setFont(&digitFont[i]); if (g->fontHeight() <= maxH && g->textWidth(txt) <= maxW) { pick = i; break; } }
    g->setFont(&digitFont[pick]); g->setTextColor(col, bg); g->drawString(txt, x, y); g->setTextColor(col);
    return;
  }
  g->setFont(&fonts::Font7); g->setTextSize(1);
  float sc = min((float)maxH / 48.0f, (float)maxW / max(1, (int)g->textWidth(txt)));
  g->setTextSize(sc); g->setTextColor(col); g->drawString(txt, x, y); g->setTextSize(1);
}
void drawBigNumber(const char *txt, int right, int y, int maxW, uint32_t col) {
  drawNumber(txt, right, y + DISP_ROW_H / 2, maxW, DISP_ROW_H - 8, middle_right, col, 0x111a2e);
}
// The sub label (DIA, LONG, FWD) is near white: it stays readable whatever the panel's colour order.
void drawRowLabel(int y, const char *name, const char *sub, uint32_t col) {
  g->fillRoundRect(2, y + 2, 316, DISP_ROW_H - 4, 6, 0x111a2e); g->fillRect(2, y + 2, 4, DISP_ROW_H - 4, col);
  g->setTextDatum(top_left); g->setFont(&fonts::Font4); g->setTextColor(col); g->drawString(name, 11, y + 12);
  g->setFont(&fonts::Font2); g->setTextColor(0xe6edf7); g->drawString(sub, 11, y + 52);
}
void drawAxis(int y, const char *name, const char *sub, float v) {
  drawRowLabel(y, name, sub, 0x38bdf8);
  char b[16]; snprintf(b, sizeof b, "%.4f", v);
  drawBigNumber(b, 314, y, 314 - DISP_LABEL_W - 4, 0x38bdf8);
}

// ---------------- touch ----------------
// The touch panel is read raw (12-bit) and mapped to the screen with the firmware's own three-point calibration:
// raw readings taken while the user pressed crosses at the top-left, top-right and bottom-left of the screen in
// rotation 1. Those three points fix which raw axis is which, their direction and their scale. Flipped mounting
// is handled afterwards by turning the result 180 degrees.
const int CAL_PT[3][2] = {{24, 24}, {296, 24}, {24, 216}};
bool touchCalibrated() { return S.tcal[0] || S.tcal[1] || S.tcal[2] || S.tcal[3]; }
bool rawToScreen(int32_t rx, int32_t ry, bool flip, int &x, int &y) {
  const int16_t *c = S.tcal;
  bool swap = abs(c[2] - c[0]) < abs(c[3] - c[1]);    // top-left to top-right moved mostly along raw y: axes are swapped
  int u = swap ? ry : rx, v = swap ? rx : ry;
  int u0 = swap ? c[1] : c[0], v0 = swap ? c[0] : c[1];
  int u1 = swap ? c[3] : c[2], v1 = swap ? c[4] : c[5];
  if (u1 == u0 || v1 == v0) return false;
  x = CAL_PT[0][0] + (u - u0) * (CAL_PT[1][0] - CAL_PT[0][0]) / (u1 - u0);
  y = CAL_PT[0][1] + (v - v0) * (CAL_PT[2][1] - CAL_PT[0][1]) / (v1 - v0);
  if (flip) { x = 319 - x; y = 239 - y; }
  x = constrain(x, 0, 319); y = constrain(y, 0, 239);
  return true;
}
void waitTouchRelease() {
  int32_t rx, ry;
  while (lcd.getTouchRaw(&rx, &ry)) vTaskDelay(pdMS_TO_TICKS(20));
}
// Drawn straight to the panel, in rotation 1, whatever the mounting.
void calibrateTouch(bool flip) {
  lcd.setRotation(1);
  int16_t c[6];
  for (int i = 0; i < 3; i++) {
    lcd.fillScreen(0x0a1020);
    lcd.setTextDatum(middle_center); lcd.setTextColor(0xe6edf7); lcd.setFont(&fonts::Font4);
    lcd.drawString("Touch calibration", 160, 108);
    char m[32]; snprintf(m, sizeof m, "Press the cross, %d of 3", i + 1);
    lcd.setFont(&fonts::Font2); lcd.drawString(m, 160, 138);
    int cx = CAL_PT[i][0], cy = CAL_PT[i][1];
    lcd.drawFastHLine(cx - 12, cy, 25, 0xfbbf24); lcd.drawFastVLine(cx, cy - 12, 25, 0xfbbf24); lcd.drawCircle(cx, cy, 8, 0xfbbf24);
    waitTouchRelease();
    int32_t sx = 0, sy = 0, n = 0;
    while (n < 8) {                                     // average 8 readings of one steady press
      int32_t rx, ry;
      if (lcd.getTouchRaw(&rx, &ry)) { sx += rx; sy += ry; n++; }
      else if (n) { sx = sy = n = 0; }                  // let go too soon: start that point again
      vTaskDelay(pdMS_TO_TICKS(15));
    }
    c[2 * i] = sx / n; c[2 * i + 1] = sy / n;
    lcd.fillCircle(cx, cy, 8, 0x22c55e);
    waitTouchRelease();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  // The three points must be well apart in raw units, or the touch panel is not answering: keep the old values then.
  bool ok = abs(c[2] - c[0]) + abs(c[3] - c[1]) > 500 && abs(c[4] - c[0]) + abs(c[5] - c[1]) > 500;
  if (ok) { memcpy(S.tcal, c, sizeof c); dispSaveReq = true; }
  lcd.fillScreen(0x0a1020); lcd.setFont(&fonts::Font4); lcd.setTextDatum(middle_center); lcd.setTextColor(ok ? 0x22c55e : 0xf59e0b);
  lcd.drawString(ok ? "Calibrated" : "No touch detected", 160, 120);
  vTaskDelay(pdMS_TO_TICKS(900));
  lcd.setRotation(flip ? 3 : 1);
}

// ---------------- on-screen menu ----------------
// A tap anywhere on the DRO opens it. Buttons are finger sized; nothing here needs the web page.
struct Btn { int x, y, w, h; };
const Btn MB[10] = {{6, 6, 152, 52}, {162, 6, 152, 52}, {6, 64, 152, 52}, {162, 64, 152, 52},
                    {6, 122, 152, 52}, {162, 122, 152, 52}, {6, 180, 74, 52}, {84, 180, 74, 52}, {162, 180, 74, 52}, {240, 180, 74, 52}};
enum { B_ZERO_X, B_ZERO_Z, B_XDIA, B_FLIP, B_SWAP, B_CAL, B_VIEW, B_INFO, B_CLOSE, B_REBOOT };
const Btn YES_BTN = {6, 150, 152, 48}, NO_BTN = {162, 150, 152, 48}, BACK_BTN = {6, 186, 308, 48};
// View screen: what fills the DRO. Five layouts and Back.
const Btn VB[6] = {{6, 6, 152, 52}, {162, 6, 152, 52}, {6, 64, 152, 52}, {162, 64, 152, 52}, {6, 122, 152, 52}, {162, 122, 152, 52}};
const char *VIEW_NAME[5] = {"Everything", "RPM only", "X only", "Z only", "X and Z"};
bool hit(const Btn &b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
void drawBtn(const Btn &b, const char *l1, const char *l2, uint32_t fill = 0x1e2a44, uint32_t txt = 0xe6edf7, bool small = false) {
  g->fillRoundRect(b.x, b.y, b.w, b.h, 8, fill);
  g->setTextDatum(middle_center); g->setTextColor(txt);
  if (l2) {
    g->setFont(&fonts::Font2); g->drawString(l1, b.x + b.w / 2, b.y + b.h / 2 - 9);
    g->setTextColor(0xfbbf24); g->drawString(l2, b.x + b.w / 2, b.y + b.h / 2 + 9);
  } else { g->setFont(small ? (const lgfx::IFont *)&fonts::Font2 : (const lgfx::IFont *)&fonts::Font4); g->drawString(l1, b.x + b.w / 2, b.y + b.h / 2); }
}
void drawMenu(const DispSnap &s) {
  g->fillScreen(0x0a1020);
  drawBtn(MB[B_ZERO_X], "Zero X", nullptr, 0x0c4a6e);
  drawBtn(MB[B_ZERO_Z], "Zero Z", nullptr, 0x0c4a6e);
  drawBtn(MB[B_XDIA], "X shows", S.xDia ? "diameter" : "radius");
  drawBtn(MB[B_FLIP], "Screen", S.dispFlip ? "upside down" : "normal");
  drawBtn(MB[B_SWAP], "Swap red / blue", S.dispSwapRb ? "on" : "off");
  // Colour check in the swap button's lower right corner, clear of its text: if the letters match the
  // colours, the order is right
  const uint32_t rgb[3] = {0xff0000, 0x00ff00, 0x0000ff}; const char *ltr[3] = {"R", "G", "B"};
  g->setFont(&fonts::Font0); g->setTextDatum(middle_center);
  for (int i = 0; i < 3; i++) {
    int bx = MB[B_SWAP].x + MB[B_SWAP].w - 56 + 18 * i, by = MB[B_SWAP].y + MB[B_SWAP].h - 20;
    g->fillRoundRect(bx, by, 14, 14, 3, rgb[i]); g->setTextColor(0xffffff); g->drawString(ltr[i], bx + 7, by + 7);
  }
  drawBtn(MB[B_CAL], "Calibrate touch", nullptr);
  drawBtn(MB[B_VIEW], "View", nullptr, 0x1e2a44, 0xe6edf7, true);
  drawBtn(MB[B_INFO], "Info", nullptr, 0x1e2a44, 0xe6edf7, true);
  drawBtn(MB[B_CLOSE], "Close", nullptr, 0x1e2a44, 0xe6edf7, true);
  drawBtn(MB[B_REBOOT], "Reboot", nullptr, 0x7f1d1d, 0xe6edf7, true);
}
void drawViewMenu(const DispSnap &s) {
  g->fillScreen(0x0a1020);
  for (int i = 0; i < 5; i++) drawBtn(VB[i], VIEW_NAME[i], nullptr, i == S.dispView ? 0x0c4a6e : 0x1e2a44);
  drawBtn(VB[5], "Back", nullptr);
  g->setFont(&fonts::Font2); g->setTextDatum(top_left); g->setTextColor(0x8fa1c2);
  g->drawString("Pick what fills the screen. Tap it to switch.", 8, 188);
  g->drawString("No axis encoders fitted yet? Choose RPM only.", 8, 208);
}
void drawConfirmReboot(const DispSnap &s) {
  g->fillScreen(0x0a1020);
  g->setTextDatum(middle_center); g->setFont(&fonts::Font4); g->setTextColor(0xe6edf7);
  g->drawString("Restart the controller now?", 160, 80);
  g->setFont(&fonts::Font2); g->setTextColor(0x8fa1c2); g->drawString("The web page reconnects by itself.", 160, 110);
  drawBtn(YES_BTN, "Yes, reboot", nullptr, 0x7f1d1d);
  drawBtn(NO_BTN, "No", nullptr);
}
// Firmware, network and the run-hour counters (spindle time counts whenever it turns above 10 rpm, and is
// saved every minute, so it is the spindle's total mileage).
void drawInfo(const DispSnap &s) {
  g->fillScreen(0x0a1020);
  g->setTextDatum(top_left); g->setFont(&fonts::Font4); g->setTextColor(0xe6edf7); g->drawString("ESP32 Lathe", 8, 8);
  g->setFont(&fonts::Font2);
  int y = 46; char v[64];
  auto line = [&](const char *k, const char *val) {
    g->setTextColor(0x8fa1c2); g->drawString(k, 8, y); g->setTextColor(0xe6edf7); g->drawString(val, 118, y); y += 24;
  };
  line("Firmware", "v" FW_VERSION);
  if (s.up) snprintf(v, sizeof v, "%s   %d dBm", s.ip, s.rssi); else if (s.ap) snprintf(v, sizeof v, "own network, %s", s.ip); else snprintf(v, sizeof v, "not connected");
  line("Wi-Fi", v);
  snprintf(v, sizeof v, "http://%s.local", s.host); line("Address", v);
  snprintf(v, sizeof v, "%lu h %02lu min", (unsigned long)(s.spinS / 3600), (unsigned long)(s.spinS % 3600 / 60)); line("Spindle run", v);
  snprintf(v, sizeof v, "%lu h %02lu min", (unsigned long)(s.powS / 3600), (unsigned long)(s.powS % 3600 / 60)); line("Powered on", v);
  drawBtn(BACK_BTN, "Back", nullptr);
}
// ---------------- single-value views ----------------
// A number in the seven-segment font, as large as maxW and maxScale allow, centred on (cx, cy).
void bigCentered(const char *txt, int cx, int cy, int maxW, float maxScale, uint32_t col) {
  drawNumber(txt, cx, cy, maxW, (int)(48 * maxScale), middle_center, col, 0x0a1020);
}
void menuMark(int x, int y) { for (int i = 0; i < 3; i++) g->fillRect(x, y + 4 * i, 14, 2, 0x8fa1c2); }
const char *toolText(const DispSnap &s) { const char *t = strstr(s.tool, " + "); return t ? t + 3 : (s.tool[0] ? s.tool : "No tool"); }
// Bottom line of the single-value views: CNC and Wi-Fi state.
void footerStatus(const DispSnap &s, int y) {
  char f[64]; snprintf(f, sizeof f, "%s    %s", s.cnc ? "CNC online" : "CNC offline", s.up ? s.ip : (s.ap ? "AP 192.168.4.1" : "no Wi-Fi"));
  g->setFont(&fonts::Font2); g->setTextDatum(top_left); g->setTextColor(0x8fa1c2); g->drawString(f, 8, y);
}
// RPM only: the speed in 120 px digits, with direction, target, speed bar, tool or alarm, encoder state.
void drawRpmView(const DispSnap &s) {
  g->fillScreen(0x0a1020);
  uint32_t col = s.enc || s.rpm < 1 ? 0x22c55e : 0xf59e0b;
  g->setFont(&fonts::Font4); g->setTextDatum(top_left); g->setTextColor(0x22c55e); g->drawString("SPINDLE", 8, 6);
  g->setTextColor(0xe6edf7); g->drawString(s.fwd ? "FWD" : "REV", 136, 6);
  char tg[16]; snprintf(tg, sizeof tg, "SET %d", s.target); g->setTextDatum(top_right); g->drawString(tg, 312, 6);
  char r[8]; snprintf(r, sizeof r, "%d", (int)(s.rpm + 0.5f));
  bigCentered(r, 160, 96, 300, 2.5f, col);
  int mx = s.maxRpm > 0 ? s.maxRpm : 2000;
  g->fillRoundRect(8, 164, 304, 14, 4, 0x1e2a44);
  g->fillRoundRect(8, 164, constrain((int)(304 * s.rpm / mx), 0, 304), 14, 4, col);
  if (s.target > 0) { int tx = 8 + constrain(304 * s.target / mx, 0, 304); g->fillRect(tx - 1, 159, 3, 24, 0xe6edf7); }
  g->setFont(&fonts::Font2); g->setTextDatum(top_left); g->setClipRect(8, 190, 196, 22);
  if (s.alarms) { g->fillRoundRect(8, 190, 196, 22, 4, 0xb45309); g->setTextColor(0xffffff); g->drawString(s.alarm, 12, 193); }
  else { g->setTextColor(0xfbbf24); g->drawString(toolText(s), 8, 193); }
  g->clearClipRect();
  g->setTextDatum(top_right); g->setTextColor(s.enc ? 0x22c55e : 0x8fa1c2); g->drawString(s.enc ? "Encoder OK" : "Stopped", 312, 193);
  footerStatus(s, 216); menuMark(300, 222);
}
// X only or Z only: one axis in digits up to 120 px tall, the other axis and the spindle small underneath.
void drawAxisView(const DispSnap &s, bool isX) {
  g->fillScreen(0x0a1020);
  g->setFont(&fonts::Font4); g->setTextDatum(top_left); g->setTextColor(0x38bdf8); g->drawString(isX ? "X" : "Z", 8, 6);
  g->setTextColor(0xe6edf7); g->drawString(isX ? (s.xDia ? "DIAMETER" : "RADIUS") : "LONGITUDINAL", 40, 6);
  g->setTextDatum(top_right); g->setTextColor(0x8fa1c2); g->drawString("inch", 312, 6);
  char b[16]; snprintf(b, sizeof b, "%.4f", isX ? s.x : s.z);
  bigCentered(b, 160, 96, 304, 2.5f, 0x38bdf8);
  g->setFont(&fonts::Font4); g->setTextDatum(top_left); g->setTextColor(0x8fa1c2);
  char o[32]; snprintf(o, sizeof o, "%s %.4f", isX ? "Z" : "X", isX ? s.z : s.x); g->drawString(o, 8, 166);
  snprintf(o, sizeof o, "S %d %s", (int)(s.rpm + 0.5f), s.fwd ? "FWD" : "REV"); g->setTextDatum(top_right); g->drawString(o, 312, 166);
  g->setFont(&fonts::Font2); g->setTextDatum(top_left); g->setClipRect(8, 194, 304, 20);
  if (s.alarms) { g->setTextColor(0xf59e0b); g->drawString(s.alarm, 8, 194); } else { g->setTextColor(0xfbbf24); g->drawString(toolText(s), 8, 194); }
  g->clearClipRect();
  footerStatus(s, 216); menuMark(300, 222);
}
// X and Z: two rows of 118 px, no spindle.
void drawXZView(const DispSnap &s) {
  g->fillScreen(0x0a1020);
  const int rh = 118;
  for (int i = 0; i < 2; i++) {
    int y = i * (rh + 4);
    g->fillRoundRect(2, y + 2, 316, rh - 4, 6, 0x111a2e); g->fillRect(2, y + 2, 4, rh - 4, 0x38bdf8);
    g->setTextDatum(top_left); g->setFont(&fonts::Font4); g->setTextColor(0x38bdf8); g->drawString(i ? "Z" : "X", 11, y + 12);
    g->setFont(&fonts::Font2); g->setTextColor(0xe6edf7); g->drawString(i ? "LONG" : (s.xDia ? "DIA" : "RAD"), 11, y + 44);
    char b[16]; snprintf(b, sizeof b, "%.4f", i ? s.z : s.x);
    drawNumber(b, 314, y + rh / 2, 270, rh - 12, middle_right, 0x38bdf8, 0x111a2e);
  }
  menuMark(11, rh - 14);
}
void drawMain(const DispSnap &s);
void drawDro(const DispSnap &s) {
  switch (S.dispView) {
    case 1: drawRpmView(s); break;
    case 2: drawAxisView(s, true); break;
    case 3: drawAxisView(s, false); break;
    case 4: drawXZView(s); break;
    default: drawMain(s);
  }
}
void drawMain(const DispSnap &s) {
  g->fillScreen(0x0a1020);
  // DRO rows
  drawAxis(0, "X", s.xDia ? "DIA" : "RAD", s.x);
  drawAxis(DISP_ROW_H, "Z", "LONG", s.z);
  for (int i = 0; i < 3; i++) g->fillRect(11, 69 + 4 * i, 14, 2, 0x8fa1c2);    // menu mark: a tap opens Settings
  // Spindle row: big RPM on the left, target, speed bar and tool (or alarm) in a narrow column on the right
  const int y = 2 * DISP_ROW_H, colX = 216, colW = 96;
  drawRowLabel(y, "S", s.fwd ? "FWD" : "REV", 0x22c55e);
  char r[8]; snprintf(r, sizeof r, "%d", (int)(s.rpm + 0.5f));
  drawBigNumber(r, colX - 6, y, colX - 6 - DISP_LABEL_W - 4, 0x22c55e);
  g->setFont(&fonts::Font2); g->setTextDatum(top_left); g->setTextColor(0xe6edf7);
  char tg[16]; snprintf(tg, sizeof tg, "SET %d", s.target); g->drawString(tg, colX, y + 10);
  int mx = s.maxRpm > 0 ? s.maxRpm : 2000;
  g->fillRoundRect(colX, y + 32, colW, 10, 3, 0x1e2a44);
  g->fillRoundRect(colX, y + 32, constrain((int)(colW * s.rpm / mx), 0, colW), 10, 3, s.enc || s.rpm < 1 ? 0x22c55e : 0xf59e0b);
  if (s.target > 0) { int tx = colX + constrain(colW * s.target / mx, 0, colW); g->fillRect(tx - 1, y + 28, 3, 18, 0xe6edf7); }
  // An alarm takes priority over the tool name; both are clipped to the column.
  g->setClipRect(colX, y + 50, colW, 24);
  if (s.alarms) { g->fillRoundRect(colX, y + 50, colW, 22, 4, 0xb45309); g->setTextColor(0xffffff); g->drawString(s.alarm, colX + 3, y + 53); }
  else { g->setTextColor(0xfbbf24); const char *t = strstr(s.tool, " + "); g->drawString(t ? t + 3 : (s.tool[0] ? s.tool : "No tool"), colX, y + 53); }
  g->clearClipRect();
}
// Boot picture, drawn straight to the panel: a lathe made of rectangles and circles, the name and the version.
// If it shows, the wiring and the panel are fine whatever happens with the frame buffer afterwards. Greys stay
// grey whichever way round the panel has red and blue; only the workpiece and the chips are coloured.
void drawSplash() {
  const uint32_t steel = 0x475569, dark = 0x334155, light = 0x94a3b8, brass = 0xfbbf24;
  lcd.fillScreen(0x0a1020);
  // bed, legs and base
  lcd.fillRoundRect(28, 152, 264, 18, 3, steel);
  lcd.fillRect(44, 170, 22, 34, dark); lcd.fillRect(254, 170, 22, 34, dark); lcd.fillRect(28, 204, 264, 4, dark);
  // headstock, spindle nose, chuck with three jaws
  lcd.fillRoundRect(40, 88, 72, 66, 6, dark); lcd.fillRect(40, 96, 72, 5, light);
  lcd.fillCircle(120, 120, 24, light); lcd.fillCircle(120, 120, 17, dark);
  lcd.fillRect(117, 98, 6, 11, light); lcd.fillRect(102, 128, 11, 6, light); lcd.fillRect(127, 128, 11, 6, light);
  // workpiece from the chuck to the tailstock centre
  lcd.fillRoundRect(120, 113, 108, 14, 4, brass);
  lcd.fillTriangle(240, 112, 240, 128, 228, 120, light);
  // tailstock and its hand wheel
  lcd.fillRoundRect(240, 104, 40, 50, 4, dark); lcd.fillCircle(290, 124, 9, light); lcd.fillCircle(290, 124, 4, dark);
  // saddle, cross slide, tool post and the tool tip against the work
  lcd.fillRect(148, 142, 56, 10, light); lcd.fillRect(158, 136, 36, 6, steel); lcd.fillRect(170, 128, 16, 8, light);
  lcd.fillRect(178, 125, 4, 4, 0xe6edf7);
  // a few chips flying off
  lcd.fillCircle(188, 108, 2, brass); lcd.fillCircle(196, 100, 2, brass); lcd.fillCircle(204, 106, 2, brass); lcd.fillCircle(192, 94, 1, brass);
  // name and version
  lcd.setTextDatum(middle_center); lcd.setTextColor(0xe6edf7); lcd.setFont(&fonts::Font4); lcd.drawString("ESP32 Lathe", 160, 40);
  lcd.setFont(&fonts::Font2); lcd.setTextColor(0x8fa1c2); lcd.drawString("firmware v" FW_VERSION, 160, 66);
}
// Draws a whole frame off screen, then sends it in one go, so nothing flickers.
void displayTask(void *) {
  lcd.init(); bool flip = S.dispFlip; lcd.setRotation(flip ? 3 : 1);
  if (S.dispSwapRb) lcd.setColorSwap(true);
  loadDigitFonts();
  drawSplash();
  vTaskDelay(pdMS_TO_TICKS(2000));
  // Frame buffer: 16-bit in PSRAM, else 8-bit in internal RAM, else none (draw straight to the panel).
  frame.setColorDepth(16); frame.setPsram(true);
  bool buffered = frame.createSprite(320, 240) != nullptr;
  if (!buffered) { frame.setPsram(false); frame.setColorDepth(8); buffered = frame.createSprite(320, 240) != nullptr; }
  g = buffered ? static_cast<lgfx::LovyanGFX *>(&frame) : static_cast<lgfx::LovyanGFX *>(&lcd);
  Serial.printf("display: %s\n", buffered ? "frame buffer allocated" : "no memory for a frame buffer, drawing directly");
  lcd.fillScreen(TFT_BLACK);
  enum { M_MAIN, M_MENU, M_VIEW, M_INFO, M_REBOOT } mode = M_MAIN;
  bool down = false; int px = 0, py = 0;                 // press in progress, and where it started
  uint32_t lastDraw = 0, lastTap = 0;
  for (;;) {
    if (flip != S.dispFlip) { flip = S.dispFlip; lcd.setRotation(flip ? 3 : 1); }
    uint32_t now = millis();
    // Touch: act on the release, at the point first pressed, so a finger sliding off a button does nothing.
    int32_t rx, ry; int x, y; bool tap = false;
    bool touched = lcd.getTouchRaw(&rx, &ry) && (!touchCalibrated() || rawToScreen(rx, ry, flip, x, y));
    if (touched && !down) { down = true; px = touchCalibrated() ? x : -1; py = touchCalibrated() ? y : -1; }
    else if (!touched && down) { down = false; tap = true; }
    if (tap) {
      lastTap = now;
      if (!touchCalibrated()) { calibrateTouch(flip); mode = M_MENU; }
      else if (mode == M_MAIN) mode = M_MENU;
      else if (mode == M_MENU) {
        if (hit(MB[B_ZERO_X], px, py)) zeroReq[0] = true;
        else if (hit(MB[B_ZERO_Z], px, py)) zeroReq[1] = true;
        else if (hit(MB[B_XDIA], px, py)) { S.xDia = !S.xDia; dispSaveReq = true; }
        else if (hit(MB[B_FLIP], px, py)) { S.dispFlip = !S.dispFlip; dispSaveReq = true; }
        else if (hit(MB[B_SWAP], px, py)) { S.dispSwapRb = !S.dispSwapRb; lcd.setColorSwap(S.dispSwapRb); dispSaveReq = true; }
        else if (hit(MB[B_CAL], px, py)) calibrateTouch(flip);
        else if (hit(MB[B_VIEW], px, py)) mode = M_VIEW;
        else if (hit(MB[B_INFO], px, py)) mode = M_INFO;
        else if (hit(MB[B_CLOSE], px, py)) mode = M_MAIN;
        else if (hit(MB[B_REBOOT], px, py)) mode = M_REBOOT;
      } else if (mode == M_VIEW) {
        for (int i = 0; i < 5; i++) if (hit(VB[i], px, py)) { S.dispView = i; dispSaveReq = true; mode = M_MAIN; }   // show it at once
        if (hit(VB[5], px, py)) mode = M_MENU;
      } else if (mode == M_INFO) mode = M_MENU;                 // any tap goes back
      else if (mode == M_REBOOT) {
        if (hit(YES_BTN, px, py)) {
          lcd.fillScreen(0x0a1020); lcd.setTextDatum(middle_center); lcd.setFont(&fonts::Font4); lcd.setTextColor(0xe6edf7);
          lcd.drawString("Rebooting...", 160, 120); delay(300); ESP.restart();
        }
        if (hit(NO_BTN, px, py)) mode = M_MENU;
      }
    }
    if (mode != M_MAIN && now - lastTap > MENU_TIMEOUT_MS) mode = M_MAIN;   // left open: back to the DRO
    if (tap || now - lastDraw >= DISP_FRAME_MS) {
      lastDraw = now;
      DispSnap s; xSemaphoreTake(dispLock, portMAX_DELAY); s = dispShared; xSemaphoreGive(dispLock);
      if (mode == M_MAIN) drawDro(s); else if (mode == M_MENU) drawMenu(s); else if (mode == M_VIEW) drawViewMenu(s); else if (mode == M_INFO) drawInfo(s); else drawConfirmReboot(s);
      if (buffered) frame.pushSprite(0, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
  }
}

void controlLoop(float dt) {
  // measure
  noInterrupts();
  uint32_t n = encCount, t0 = encFirstUs, t1 = encLastUs; int32_t d = encDir;
  encCount = 0; encDir = 0;
  interrupts();
  static uint32_t lastPulse = 0;
  if (n) { lastPulse = t1; spindleFwd = d >= 0; }
  encOk = (micros() - lastPulse) < 1500000UL;
  float r = (n >= 2 && t1 > t0) ? ((n - 1) / S.ppr) / ((t1 - t0) / 60.0e6f) : 0;
  rpm = encOk ? r : 0;
  rpmShown = 0.6f * rpmShown + 0.4f * rpm;

  // target
  if (webTarget >= 0) targetRpm = webTarget;
  else if (S.knobEnabled) {
    long acc = 0; for (int i = 0; i < 16; i++) acc += analogRead(PIN_POT); int pot = acc / 16;
    int t = pot > 80 ? S.minRpm + (long)(pot - 80) * (S.maxRpm - S.minRpm) / (4095 - 80) : 0;
    targetRpm = (t / 10) * 10;
  } else targetRpm = 0;

  // PI
  if (targetRpm == 0) { duty = 0; integ = 0; }
  else if (encOk) {
    float err = targetRpm - rpm; integ += err * dt; integ = constrain(integ, 0, 1.0f / S.ki);
    duty = S.kp * err + S.ki * integ;
  } else duty += 0.02f;                               // spindle not turning yet: ramp until pulses arrive
  duty = constrain(duty, 0.0f, 1.0f);
  ledcWrite(PWM_CH, (int)(duty * PWM_MAX));
  setAlarm("enc", "Encoder signal lost while spindle commanded", targetRpm > 0 && !encOk && duty > 0.5f);
}

// ---------------- state JSON ----------------
String stateJson() {
  JsonDocument d;
  d["x"] = xPos; d["z"] = zPos; d["rpm"] = (int)(rpmShown + 0.5f); d["target"] = targetRpm;
  d["fwd"] = spindleFwd; d["load"] = (int)(duty * 100); d["enc"] = encOk; d["cnc"] = cncOnline;
  d["rec"] = recording; d["play"] = playing; d["recName"] = recName; d["recLen"] = rec.size();
  d["recT"] = recording ? millis() - recStart : (playing && playIdx < rec.size() ? rec[playIdx].t : 0);
  d["recDur"] = rec.empty() ? 0 : rec.back().t;
  d["spinH"] = spindleSeconds / 3600.0f; d["powH"] = powerSeconds / 3600.0f;
  d["rssi"] = WiFi.RSSI(); d["ip"] = WiFi.localIP().toString(); d["host"] = S.host; d["fw"] = FW_VERSION;
  d["knob"] = S.knobEnabled; d["webOverride"] = webTarget >= 0;
  xSemaphoreTake(dispLock, portMAX_DELAY); d["tool"] = toolName; d["toolId"] = toolId; xSemaphoreGive(dispLock);
  JsonArray a = d["alarms"].to<JsonArray>();
  for (auto &al : alarms) { JsonObject o = a.add<JsonObject>(); o["t"] = hhmmss(al.t); o["msg"] = al.msg; }
  String out; serializeJson(d, out); return out;
}

void onWsMessage(AsyncWebSocketClient *c, const String &msg) {
  JsonDocument d; if (deserializeJson(d, msg)) return;
  String cmd = d["cmd"] | "";
  if (cmd == "target")      { webTarget = d["rpm"] | 0; logEvent("spindle", "Spindle target " + String(webTarget) + " rpm (web)"); }
  else if (cmd == "knob")   { webTarget = -1; logEvent("spindle", "Spindle target back to knob"); }
  else if (cmd == "zeroX")  { zeroReq[0] = true; logEvent("zero", "Axis zero: X"); }
  else if (cmd == "zeroZ")  { zeroReq[1] = true; logEvent("zero", "Axis zero: Z"); }
  else if (cmd == "recStart") { rec.clear(); recording = true; playing = false; recStart = millis(); recName = d["name"] | "recording"; logEvent("rec", "Recording started (" + recName + ")"); }
  else if (cmd == "recStop")  { recording = false; playing = false; logEvent("rec", "Recording stopped, " + String(rec.size()) + " samples"); }
  else if (cmd == "recSave")  { recSave(recName); }
  else if (cmd == "recLoad")  { if (recLoad(d["name"] | "")) logEvent("rec", "Loaded " + recName); }
  else if (cmd == "play")     { if (!rec.empty()) { playing = true; recording = false; playIdx = 0; playStart = millis(); playSpeed = d["speed"] | 1.0f; logEvent("play", "Playback started: " + recName); } }
  else if (cmd == "stop")     { if (playing) { playing = false; webTarget = -1; logEvent("play", "Playback stopped"); } }
  else if (cmd == "notes")    { jobNotes = d["text"] | ""; prefs.begin("lathe", false); prefs.putString("notes", jobNotes); prefs.end(); logEvent("note", "Job notes saved"); }
  else if (cmd == "ledTest")  { ledTestUntil = millis() + LED_TEST_MS; }
  else if (cmd == "toolLoad") { xSemaphoreTake(dispLock, portMAX_DELAY); toolId = d["id"] | ""; toolName = d["name"] | ""; xSemaphoreGive(dispLock); prefs.begin("lathe", false); prefs.putString("toolId", toolId); prefs.putString("toolName", toolName); prefs.end();
                                logEvent("tool", toolName.length() ? "Tool loaded: " + toolName : String("Tool unloaded")); }
  else if (cmd == "events")   { JsonDocument e; JsonArray a = e["events"].to<JsonArray>();
                                for (auto &ev : events) { JsonObject o = a.add<JsonObject>(); o["t"] = hhmmss(ev.t); o["type"] = ev.type; o["msg"] = ev.msg; }
                                e["notes"] = jobNotes; String out; serializeJson(e, out); c->text(out); }
  else if (cmd == "recData")  { JsonDocument e; JsonArray a = e["samples"].to<JsonArray>();
                                size_t step = rec.size() / 600 + 1;
                                for (size_t i = 0; i < rec.size(); i += step) { JsonArray s = a.add<JsonArray>(); s.add(rec[i].t); s.add(rec[i].x); s.add(rec[i].z); s.add(rec[i].rpm); }
                                e["recName"] = recName; String out; serializeJson(e, out); c->text(out); }
}

// ---------------- setup ----------------
void setupWeb() {
  ws.onEvent([](AsyncWebSocket *, AsyncWebSocketClient *c, AwsEventType t, void *arg, uint8_t *data, size_t len) {
    if (t == WS_EVT_CONNECT) c->text(stateJson());
    else if (t == WS_EVT_DATA) { AwsFrameInfo *i = (AwsFrameInfo *)arg; if (i->final && i->index == 0 && i->len == len) onWsMessage(c, String((char *)data, len)); }
  });
  server.addHandler(&ws);
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *r) {
    JsonDocument d; d["ssid"] = S.ssid; d["host"] = S.host; d["ppr"] = S.ppr;
    d["xPpr"] = S.xPpr; d["zPpr"] = S.zPpr; d["xLead"] = S.xLead; d["zLead"] = S.zLead; d["xDia"] = S.xDia;
    d["xInv"] = S.xInv; d["zInv"] = S.zInv; d["maxRpm"] = S.maxRpm; d["minRpm"] = S.minRpm; d["kp"] = S.kp; d["ki"] = S.ki;
    d["xMin"] = S.xMin; d["xMax"] = S.xMax; d["zMin"] = S.zMin; d["zMax"] = S.zMax; d["knob"] = S.knobEnabled; d["manifest"] = S.manifest; d["fw"] = FW_VERSION;
    d["ledRgb"] = S.ledRgb; d["ledBlue"] = S.ledBlue; d["blueLow"] = S.blueLow; d["dispFlip"] = S.dispFlip;
    String out; serializeJson(d, out); r->send(200, "application/json", out);
  });
  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument d; if (deserializeJson(d, data, len)) { r->send(400); return; }
      if (d["ssid"].is<const char*>()) strlcpy(S.ssid, d["ssid"].as<const char*>(), sizeof S.ssid);
      if (d["pass"].is<const char*>() && strlen(d["pass"].as<const char*>())) strlcpy(S.pass, d["pass"].as<const char*>(), sizeof S.pass);
      if (d["host"].is<const char*>()) strlcpy(S.host, d["host"].as<const char*>(), sizeof S.host);
      if (d["manifest"].is<const char*>()) strlcpy(S.manifest, d["manifest"].as<const char*>(), sizeof S.manifest);
      S.ppr = d["ppr"] | S.ppr;
      { float v;                                  // ignore zero or negative values, which would break the scale
        v = d["xPpr"] | S.xPpr;  if (v > 0) S.xPpr = v;   v = d["zPpr"] | S.zPpr;  if (v > 0) S.zPpr = v;
        v = d["xLead"] | S.xLead; if (v > 0) S.xLead = v; v = d["zLead"] | S.zLead; if (v > 0) S.zLead = v; }
      S.xDia = d["xDia"] | S.xDia;
      S.xInv = d["xInv"] | S.xInv; S.zInv = d["zInv"] | S.zInv; S.maxRpm = d["maxRpm"] | S.maxRpm; S.minRpm = d["minRpm"] | S.minRpm;
      S.kp = d["kp"] | S.kp; S.ki = d["ki"] | S.ki; S.xMin = d["xMin"] | S.xMin; S.xMax = d["xMax"] | S.xMax; S.zMin = d["zMin"] | S.zMin; S.zMax = d["zMax"] | S.zMax;
      S.knobEnabled = d["knob"] | S.knobEnabled;
      int lr = d["ledRgb"] | S.ledRgb, lb = d["ledBlue"] | S.ledBlue;
      if (ledPinOk(lr, true) && ledPinOk(lb, false) && (lb < 0 || lb != lr)) { S.ledRgb = lr; S.ledBlue = lb; }
      S.blueLow = d["blueLow"] | S.blueLow; S.dispFlip = d["dispFlip"] | S.dispFlip;
      saveSettings(); logEvent("settings", "Settings saved"); r->send(200, "text/plain", "ok");
      if (d["reboot"] | false) { delay(300); ESP.restart(); }
    });
  server.on("/api/tools", HTTP_GET, [](AsyncWebServerRequest *r) {
    JsonDocument d; d["tools"] = serialized(toolLib); d["loaded"] = toolId; d["name"] = toolName;
    String out; serializeJson(d, out); r->send(200, "application/json", out);
  });
  server.on("/api/tools", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
      if (total > TOOL_JSON_MAX) { if (!index) r->send(413, "text/plain", "tool list too large"); return; }
      static String body;                             // the body can arrive in several chunks
      if (!index) { body = ""; body.reserve(total); }
      for (size_t i = 0; i < len; i++) body += (char)data[i];
      if (index + len < total) return;
      JsonDocument d; if (deserializeJson(d, body) || !d["tools"].is<JsonArray>()) { r->send(400, "text/plain", "bad tool list"); return; }
      String out; serializeJson(d["tools"], out);
      prefs.begin("lathe", false); size_t saved = prefs.putString("tools", out); prefs.end();
      if (!saved) { r->send(500, "text/plain", "could not store tools"); return; }
      toolLib = out; broadcast("{\"toolsChanged\":true}"); r->send(200, "text/plain", "ok");
    });
  server.on("/api/recordings", HTTP_GET, [](AsyncWebServerRequest *r) {
    JsonDocument d; JsonArray a = d["files"].to<JsonArray>();
    File dir = LittleFS.open("/rec"); if (dir) { File f; while ((f = dir.openNextFile())) { String n = f.name(); n.replace(".csv", ""); a.add(n); } }
    String out; serializeJson(d, out); r->send(200, "application/json", out);
  });
  server.on("/api/recording.csv", HTTP_GET, [](AsyncWebServerRequest *r) {
    String n = r->hasParam("name") ? r->getParam("name")->value() : recName;
    r->send(LittleFS, "/rec/" + n + ".csv", "text/csv", true);
  });
  server.on("/api/recording.nc", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (r->hasParam("name")) recLoad(r->getParam("name")->value());
    AsyncWebServerResponse *res = r->beginResponse(200, "text/plain", recToGcode());
    res->addHeader("Content-Disposition", "attachment; filename=\"" + recName + ".nc\""); r->send(res);
    logEvent("save", "Recording exported (" + recName + ".nc)");
  });
  server.on("/api/thread.nc", HTTP_GET, [](AsyncWebServerRequest *r) {
    float pitch = r->getParam("pitch")->value().toFloat(), major = r->getParam("major")->value().toFloat(), len = r->getParam("len")->value().toFloat();
    AsyncWebServerResponse *res = r->beginResponse(200, "text/plain", threadProgram(pitch, major, len, false));
    res->addHeader("Content-Disposition", "attachment; filename=\"thread_" + String(major, 1) + "x" + String(pitch, 2) + ".nc\""); r->send(res);
  });
  server.on("/api/check-update", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!strlen(S.manifest)) { r->send(200, "application/json", "{\"version\":\"\"}"); return; }
    HTTPClient h; h.begin(S.manifest); int code = h.GET(); String body = code == 200 ? h.getString() : "{\"version\":\"\"}"; h.end();
    r->send(200, "application/json", body);
  });
  // OTA: POST firmware.bin as multipart "firmware"
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *r) {
      bool ok = !Update.hasError(); r->send(200, "text/plain", ok ? "OK, rebooting" : "FAILED");
      if (ok) { logEvent("fw", "Firmware updated, rebooting"); delay(500); ESP.restart(); }
    }, [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) { logEvent("fw", "Firmware upload: " + filename); Update.begin(UPDATE_SIZE_UNKNOWN); }
      if (len) Update.write(data, len);
      if (final) Update.end(true);
    });
  server.begin();
}

void setupWifi() {
  WiFi.setHostname(S.host);
  if (strlen(S.ssid)) { WiFi.mode(WIFI_STA); WiFi.begin(S.ssid, S.pass);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250); }
  if (WiFi.status() != WL_CONNECTED) {               // no network yet: open an AP so the dashboard is reachable
    WiFi.mode(WIFI_AP_STA); WiFi.softAP("ESP32-Lathe", "lathe1234");
    logEvent("wifi", "Setup AP 'ESP32-Lathe' at 192.168.4.1");
  } else logEvent("wifi", "Wi-Fi connected " + WiFi.localIP().toString());
  MDNS.begin(S.host);
}

void setup() {
  Serial.begin(115200);
  loadSettings();
  prefs.begin("lathe", true); spindleSeconds = prefs.getUInt("spinS", 0); powerSeconds = prefs.getUInt("powS", 0); jobNotes = prefs.getString("notes", "");
  toolLib = prefs.getString("tools", "[]"); toolId = prefs.getString("toolId", ""); toolName = prefs.getString("toolName", ""); prefs.end();
  LittleFS.begin(true);
  for (int p : {PIN_ENC_A, PIN_ENC_B, PIN_ENC_Z, PIN_XA, PIN_XB, PIN_ZA, PIN_ZB, PIN_CP}) pinMode(p, INPUT_PULLUP);
  attachInterrupt(PIN_ENC_A, isrEncA, RISING);
  axisSetup(PCNT_UNIT_0, PIN_XA, PIN_XB);
  axisSetup(PCNT_UNIT_1, PIN_ZA, PIN_ZB);
  xTaskCreatePinnedToCore(axisTask, "axes", 2048, nullptr, 5, nullptr, 1);
  attachInterrupt(PIN_CP, isrCp, RISING);
  analogReadResolution(12);
  ledcSetup(PWM_CH, 2000, PWM_BITS); ledcAttachPin(PIN_PWM, PWM_CH); ledcWrite(PWM_CH, 0);
  dispLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(displayTask, "display", 6144, nullptr, 1, nullptr, 0);
  if (!ledPinOk(S.ledRgb, true)) S.ledRgb = -1;   // never drive a pin from a bad saved value
  if (!ledPinOk(S.ledBlue, false) || S.ledBlue == S.ledRgb) S.ledBlue = -1;
  xTaskCreatePinnedToCore(ledTask, "leds", 3072, nullptr, 1, nullptr, 0);
  setupWifi(); setupWeb();
  logEvent("ok", "Machine ready (idle)");
}

void loop() {
  static uint32_t lastCtl = 0, lastTx = 0, lastRec = 0, lastSec = 0;
  uint32_t now = millis();

  if (now - lastCtl >= 100) { controlLoop((now - lastCtl) / 1000.0f); lastCtl = now; }

  // DRO
  int64_t xs, zs; axisRead(xs, zs);
  xPos = (S.xInv ? -xs : xs) / axCountsPerIn(0); zPos = (S.zInv ? -zs : zs) / axCountsPerIn(1);
  cncOnline = (micros() - cpLastUs) < 200000UL;
  { static int64_t lastXs = 0, lastZs = 0;        // any step, spindle turning, recording or playback counts as reading
    if (xs != lastXs || zs != lastZs) { ledActivityMs = now; if (!ledActivityMs) ledActivityMs = 1; lastXs = xs; lastZs = zs; }
    ledActive = rpm > 10 || recording || playing; }
  setAlarm("xlim", "Tool approaching soft limit (X)", xPos < S.xMin + 0.05f || xPos > S.xMax - 0.05f);
  setAlarm("zlim", "Tool approaching soft limit (Z)", zPos < S.zMin + 0.05f || zPos > S.zMax - 0.05f);

  // recorder / playback
  if (recording && now - lastRec >= 100) { rec.push_back({now - recStart, xPos, zPos, rpm, duty}); lastRec = now; if (rec.size() > 36000) recording = false; }
  if (playing) {
    uint32_t t = (uint32_t)((now - playStart) * playSpeed);
    while (playIdx < rec.size() && rec[playIdx].t < t) playIdx++;
    if (playIdx >= rec.size()) { playing = false; webTarget = -1; logEvent("play", "Playback finished"); }
    else webTarget = (int)rec[playIdx].rpm;                           // replay the spindle profile
  }

  // run hours, display, websocket
  if (now - lastSec >= 1000) {
    lastSec = now; powerSeconds++; if (rpm > 10) spindleSeconds++;
    if (now - lastHourSave >= 60000) { lastHourSave = now; prefs.begin("lathe", false); prefs.putUInt("spinS", spindleSeconds); prefs.putUInt("powS", powerSeconds); prefs.end(); }
    static bool wasUp = true; bool up = WiFi.status() == WL_CONNECTED;
    if (up && !wasUp) logEvent("wifi", "Wi-Fi reconnected (" + WiFi.localIP().toString() + ")"); wasUp = up;
  }
  if (now - lastTx >= 250) { lastTx = now; dispSnapshot(); broadcast(stateJson()); ws.cleanupClients(); }
  if (dispSaveReq) { dispSaveReq = false; saveSettings(); }      // a change made on the screen's menu
}
