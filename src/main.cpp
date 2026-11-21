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
#if __has_include("web_page.h")
#include "web_page.h"       // Arduino IDE build only: the page, gzipped (tools/make_arduino.py)
#endif

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
  char  ssid[33] = "";            // set it in Settings (web page); never put a network name in the source
  char  pass[65] = "";            // set it in Settings; the source is public
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
  bool  dispBlack = false;             // black background everywhere instead of dark blue
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
  S.dispBlack = prefs.getBool("dblack", S.dispBlack);
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
  prefs.putBool("dswap", S.dispSwapRb); prefs.putBool("dblack", S.dispBlack); prefs.putUChar("dview", S.dispView); prefs.putBytes("tcal", S.tcal, sizeof S.tcal);
  prefs.end();
}

// ---------------- live state ----------------
volatile uint32_t encCount = 0, encFirstUs = 0, encLastUs = 0;
volatile int32_t  encDir = 0;
volatile uint32_t encTotal = 0, zAt = 0, zRevs = 0, zMissed = 0, zExtra = 0;   // see isrEncZ()
volatile int zPpr = 100; volatile bool zFirst = true;
volatile int accelMs = -1; volatile bool accelBusy = false;                    // time the spindle took to reach the last new speed
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
  encLastUs = now; encCount++; encTotal = encTotal + 1;
  encDir += digitalRead(PIN_ENC_B) ? 1 : -1;
}
// Spindle Diagnostics: the encoder's index (Z) gives one pulse a turn, so the A pulses between two of them must
// equal the encoder's lines per turn. Fewer means pulses were missed, more means noise was counted as pulses.
// A count that is off by more than half a turn is a glitch on the index itself and is left out.
void IRAM_ATTR isrEncZ() {
  uint32_t c = encTotal - zAt; zAt = encTotal;
  if (zFirst) { zFirst = false; return; }
  int d = (int)c - zPpr;
  if (d > zPpr / 2 || d < -zPpr / 2) return;
  zRevs = zRevs + 1;
  if (d < 0) zMissed = zMissed + (uint32_t)(-d); else zExtra = zExtra + (uint32_t)d;
}
void diagReset() { noInterrupts(); zAt = encTotal; zFirst = true; zRevs = 0; zMissed = 0; zExtra = 0; interrupts(); }
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
// Speed typed on the touch screen. The knob stays the start/stop: at zero the spindle is off, turned up it
// runs at this speed instead of the knob position. 0 = no preset, the knob sets the speed.
volatile int screenRpm = 0;
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
// The same program the page's Thread Wizard writes with its default choices: flank infeed at 29.5 degrees, pass
// depths that take the same amount of metal each time, a stop to try the nut about three quarters of the way,
// two spring passes, and the tool always moved out in X before it travels along Z.
String threadProgram(const String &name, float pitchMm, float majorMm, float lengthIn, int passes, int rpm) {
  const float tan295 = 0.5658f;
  passes = constrain(passes, 3, 20);
  float H = 0.61343f * pitchMm;                                // thread depth (ISO) in mm
  float K = pitchMm / 25.4f, startZ = 0.2f, clearX = (majorMm + 2) / 25.4f;
  auto depth = [&](int i) { return H * sqrtf((i + 1) / (float)passes); };
  String g = "(" + name + " external thread, " + String(majorMm, 2) + " x " + String(pitchMm, 3) + " mm, " + String(lengthIn, 3) + " in long)\n";
  g += "(INCH UCCNC profile - X in diameter - Z0 = end face - blank " + String((majorMm - 0.05f) / 25.4f, 4) + " in)\n";
  g += "(" + String(passes) + " passes + 2 spring passes - first pass " + String(depth(0) / 25.4f, 4) + " in - start " + String(startZ, 3) + " in in front of the end face)\n";
  g += "G90 G18 G40\nM3 S" + String(rpm) + "\nG4 P2\nG0 X" + String(clearX, 4) + "\nG0 Z" + String(startZ, 4) + "\n";
  auto pass = [&](const String &lbl, float d) {
    g += "(" + lbl + " depth " + String(d / 25.4f, 4) + ")\n";
    g += "G0 Z" + String(startZ - d * tan295 / 25.4f, 4) + "\nG0 X" + String((majorMm - 2 * d) / 25.4f, 4) + "\n";
    g += "G33 Z-" + String(lengthIn, 3) + " K" + String(K, 6) + " Q0\nG0 X" + String(clearX, 4) + "\n";
  };
  int fit = (passes * 8 + 5) / 11;
  for (int i = 0; i < passes; i++) {
    if (i == fit && fit > 0) g += "(FIT CHECK)\nG0 Z0.400\nM5\nM0\nM3 S" + String(rpm) + "\nG4 P2\nG0 Z" + String(startZ, 4) + "\n";
    pass("PASS " + String(i + 1), depth(i));
  }
  pass("SPRING 1", H); pass("SPRING 2", H);
  g += "G0 Z0.400\nM5\nM30\n";
  return g;
}

// ---------------- job from the touch screen ----------------
// The ESP32 cannot write a file onto the PC, so it works the other way round: SEND on the Thread Assist screen
// keeps the program here, and the PC fetches it from /api/job.nc. The UCCNC macro from /api/uccnc-macro does
// that and loads it; the page's Thread Wizard shows a Download link for it too. Kept until the next reboot.
String jobText, jobName;
volatile uint32_t jobId = 0;
SemaphoreHandle_t jobLock;
void jobSet(const String &name, const String &text) {
  xSemaphoreTake(jobLock, portMAX_DELAY); jobName = name; jobText = text; jobId = jobId + 1; xSemaphoreGive(jobLock);
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
// Arduino-ESP32 core 3.x replaced ledcSetup/ledcAttachPin with ledcAttach, and its ledcWrite takes the pin
// instead of the channel. These two keep one source building on 2.0.x (PlatformIO) and 3.x (current Arduino IDE).
void pwmBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttachChannel(PIN_PWM, 2000, PWM_BITS, PWM_CH);
#else
  ledcSetup(PWM_CH, 2000, PWM_BITS); ledcAttachPin(PIN_PWM, PWM_CH);
#endif
}
void pwmWrite(uint32_t v) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_PWM, v);
#else
  ledcWrite(PWM_CH, v);
#endif
}
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
  int preset, load, minRpm;                            // screen preset (0 = knob), drive in percent
  uint32_t zRevs, zMissed, zExtra; int accelMs; bool accelBusy;   // Spindle Diagnostics
};
// Spindle history for the RPM view's graph: speed, target and drive four times a second, the last 37 s.
// Written by loop(), read by the display task; a sample caught half-written only costs one pixel for one frame.
const int HIST_N = 150;
uint16_t histRpm[HIST_N]; uint8_t histLoad[HIST_N];
volatile int histHead = 0, histCount = 0;
void histPush() {
  int h = histHead;
  histRpm[h] = (uint16_t)constrain((int)(rpmShown + 0.5f), 0, 65000); histLoad[h] = (uint8_t)constrain((int)(duty * 100 + 0.5f), 0, 100);
  histHead = (h + 1) % HIST_N; if (histCount < HIST_N) histCount = histCount + 1;
}
DispSnap dispShared = {};
SemaphoreHandle_t dispLock;

void dispSnapshot() {
  DispSnap s = {};
  s.x = xPos; s.z = zPos; s.rpm = rpmShown; s.target = targetRpm; s.maxRpm = S.maxRpm; s.fwd = spindleFwd; s.enc = encOk;
  s.cnc = cncOnline; s.rec = recording; s.play = playing; s.up = WiFi.status() == WL_CONNECTED; s.ap = WiFi.getMode() & WIFI_AP; s.xDia = S.xDia;
  strlcpy(s.ip, (s.up ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str(), sizeof s.ip);
  s.alarms = alarms.size(); if (s.alarms) strlcpy(s.alarm, alarms[0].msg.c_str(), sizeof s.alarm);
  s.preset = screenRpm; s.load = (int)(duty * 100 + 0.5f); s.minRpm = S.minRpm;
  s.zRevs = zRevs; s.zMissed = zMissed; s.zExtra = zExtra; s.accelMs = accelMs; s.accelBusy = accelBusy; zPpr = max(1, (int)(S.ppr + 0.5f));
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
// Colour scheme: dark blue by default, or black everywhere (chosen on the Colours screen). In black mode the
// row panels are black too and keep a thin outline, so the layout still reads.
struct Theme { uint32_t bg, panel, btn, line; };
Theme theme() { return S.dispBlack ? Theme{0x000000, 0x000000, 0x1c1c1c, 0x2e2e2e} : Theme{0x0a1020, 0x111a2e, 0x1e2a44, 0}; }

// ---------------- screen artwork ----------------
// Glows, gradients and icons are too slow to draw live, so tools/make_art.py draws everything that never changes
// on a screen (background, panels, buttons, icons, fixed labels) into art/*.png, and those are linked into the
// firmware. The draw...Art functions copy the picture into the frame buffer and paint the live values on top;
// their coordinates match make_art.py. The picture for the screen in use is decoded once into its own buffer in
// PSRAM. There are two sets, for the dark blue and the black background (<name>.png and <name>_k.png). Without
// that memory or without a frame buffer the plain drawing code below is used instead, so the screens always work.
#if __has_include("art_png.h")
#include "art_png.h"
#else
extern const uint8_t art_plain_start[] asm("_binary_art_plain_png_start"), art_plain_end[] asm("_binary_art_plain_png_end");
extern const uint8_t art_rpm_start[] asm("_binary_art_rpm_png_start"), art_rpm_end[] asm("_binary_art_rpm_png_end");
extern const uint8_t art_menu_start[] asm("_binary_art_menu_png_start"), art_menu_end[] asm("_binary_art_menu_png_end");
extern const uint8_t art_keypad_start[] asm("_binary_art_keypad_png_start"), art_keypad_end[] asm("_binary_art_keypad_png_end");
extern const uint8_t art_info_start[] asm("_binary_art_info_png_start"), art_info_end[] asm("_binary_art_info_png_end");
extern const uint8_t art_view_start[] asm("_binary_art_view_png_start"), art_view_end[] asm("_binary_art_view_png_end");
extern const uint8_t art_main_start[] asm("_binary_art_main_png_start"), art_main_end[] asm("_binary_art_main_png_end");
extern const uint8_t art_xz_start[] asm("_binary_art_xz_png_start"), art_xz_end[] asm("_binary_art_xz_png_end");
extern const uint8_t art_thread_start[] asm("_binary_art_thread_png_start"), art_thread_end[] asm("_binary_art_thread_png_end");
extern const uint8_t art_diag_start[] asm("_binary_art_diag_png_start"), art_diag_end[] asm("_binary_art_diag_png_end");
extern const uint8_t art_mat_start[] asm("_binary_art_mat_png_start"), art_mat_end[] asm("_binary_art_mat_png_end");
extern const uint8_t art_numpad_start[] asm("_binary_art_numpad_png_start"), art_numpad_end[] asm("_binary_art_numpad_png_end");
extern const uint8_t art_tools_start[] asm("_binary_art_tools_png_start"), art_tools_end[] asm("_binary_art_tools_png_end");
extern const uint8_t art_pick_start[] asm("_binary_art_pick_png_start"), art_pick_end[] asm("_binary_art_pick_png_end");
extern const uint8_t art_plain_k_start[] asm("_binary_art_plain_k_png_start"), art_plain_k_end[] asm("_binary_art_plain_k_png_end");
extern const uint8_t art_rpm_k_start[] asm("_binary_art_rpm_k_png_start"), art_rpm_k_end[] asm("_binary_art_rpm_k_png_end");
extern const uint8_t art_menu_k_start[] asm("_binary_art_menu_k_png_start"), art_menu_k_end[] asm("_binary_art_menu_k_png_end");
extern const uint8_t art_keypad_k_start[] asm("_binary_art_keypad_k_png_start"), art_keypad_k_end[] asm("_binary_art_keypad_k_png_end");
extern const uint8_t art_info_k_start[] asm("_binary_art_info_k_png_start"), art_info_k_end[] asm("_binary_art_info_k_png_end");
extern const uint8_t art_view_k_start[] asm("_binary_art_view_k_png_start"), art_view_k_end[] asm("_binary_art_view_k_png_end");
extern const uint8_t art_main_k_start[] asm("_binary_art_main_k_png_start"), art_main_k_end[] asm("_binary_art_main_k_png_end");
extern const uint8_t art_xz_k_start[] asm("_binary_art_xz_k_png_start"), art_xz_k_end[] asm("_binary_art_xz_k_png_end");
extern const uint8_t art_thread_k_start[] asm("_binary_art_thread_k_png_start"), art_thread_k_end[] asm("_binary_art_thread_k_png_end");
extern const uint8_t art_diag_k_start[] asm("_binary_art_diag_k_png_start"), art_diag_k_end[] asm("_binary_art_diag_k_png_end");
extern const uint8_t art_mat_k_start[] asm("_binary_art_mat_k_png_start"), art_mat_k_end[] asm("_binary_art_mat_k_png_end");
extern const uint8_t art_numpad_k_start[] asm("_binary_art_numpad_k_png_start"), art_numpad_k_end[] asm("_binary_art_numpad_k_png_end");
extern const uint8_t art_tools_k_start[] asm("_binary_art_tools_k_png_start"), art_tools_k_end[] asm("_binary_art_tools_k_png_end");
extern const uint8_t art_pick_k_start[] asm("_binary_art_pick_k_png_start"), art_pick_k_end[] asm("_binary_art_pick_k_png_end");
#endif
enum { ART_PLAIN, ART_RPM, ART_MENU, ART_KEYPAD, ART_INFO, ART_VIEW, ART_MAIN, ART_XZ, ART_THREAD, ART_DIAG, ART_MAT, ART_NUMPAD, ART_TOOLS, ART_PICK, ART_N };
LGFX_Sprite art(&lcd);
int artLoaded = -1;
bool artOk = false, dispBuffered = false, artActive = false;
// Paints a screen's artwork as the background of this frame. false: the caller draws the plain version.
bool artBg(int id) {
  artActive = false;
  if (!artOk || !dispBuffered) return false;
  int key = id + (S.dispBlack ? ART_N : 0);
  if (artLoaded != key) {
    static const uint8_t *const st[2 * ART_N] = {art_plain_start, art_rpm_start, art_menu_start, art_keypad_start, art_info_start, art_view_start, art_main_start, art_xz_start, art_thread_start, art_diag_start, art_mat_start, art_numpad_start, art_tools_start, art_pick_start, art_plain_k_start, art_rpm_k_start, art_menu_k_start, art_keypad_k_start, art_info_k_start, art_view_k_start, art_main_k_start, art_xz_k_start, art_thread_k_start, art_diag_k_start, art_mat_k_start, art_numpad_k_start, art_tools_k_start, art_pick_k_start};
    static const uint8_t *const en[2 * ART_N] = {art_plain_end, art_rpm_end, art_menu_end, art_keypad_end, art_info_end, art_view_end, art_main_end, art_xz_end, art_thread_end, art_diag_end, art_mat_end, art_numpad_end, art_tools_end, art_pick_end, art_plain_k_end, art_rpm_k_end, art_menu_k_end, art_keypad_k_end, art_info_k_end, art_view_k_end, art_main_k_end, art_xz_k_end, art_thread_k_end, art_diag_k_end, art_mat_k_end, art_numpad_k_end, art_tools_k_end, art_pick_k_end};
    artLoaded = art.drawPng(st[key], en[key] - st[key], 0, 0) ? key : -1;
    if (artLoaded != key) return false;
  }
  art.pushSprite(&frame, 0, 0);
  artActive = true;
  return true;
}
void clearScreen(int artId = ART_PLAIN) { if (!artBg(artId)) g->fillScreen(theme().bg); }

// Smooth digits. The built-in seven-segment font scaled up 1.5x looked blocky, so the numbers now use
// anti-aliased 0-9 . - glyphs from Oswald Bold (SIL Open Font License, see fonts/OFL.txt) in three sizes:
// 120, 70 and 57 px digits. tools/make_vlw.py makes the fonts/*.vlw files from the TTF; platformio.ini's
// board_build.embed_files links them into the firmware, and they are read straight from flash.
// The Arduino IDE has no embed_files, so tools/make_arduino.py turns the same files into C arrays in fonts_vlw.h.
#if __has_include("fonts_vlw.h")
#include "fonts_vlw.h"
#else
extern const uint8_t vlw_big_start[] asm("_binary_fonts_d_big_vlw_start"), vlw_big_end[] asm("_binary_fonts_d_big_vlw_end");
extern const uint8_t vlw_mid_start[] asm("_binary_fonts_d_mid_vlw_start"), vlw_mid_end[] asm("_binary_fonts_d_mid_vlw_end");
extern const uint8_t vlw_small_start[] asm("_binary_fonts_d_small_vlw_start"), vlw_small_end[] asm("_binary_fonts_d_small_vlw_end");
extern const uint8_t vlw_tbig_start[] asm("_binary_fonts_t_big_vlw_start"), vlw_tbig_end[] asm("_binary_fonts_t_big_vlw_end");
extern const uint8_t vlw_tsmall_start[] asm("_binary_fonts_t_small_vlw_start"), vlw_tsmall_end[] asm("_binary_fonts_t_small_vlw_end");
extern const uint8_t vlw_rpm_start[] asm("_binary_fonts_d_rpm_vlw_start"), vlw_rpm_end[] asm("_binary_fonts_d_rpm_vlw_end");
extern const uint8_t vlw_tmid_start[] asm("_binary_fonts_t_mid_vlw_start"), vlw_tmid_end[] asm("_binary_fonts_t_mid_vlw_end");
extern const uint8_t vlw_txl_start[] asm("_binary_fonts_t_xl_vlw_start"), vlw_txl_end[] asm("_binary_fonts_t_xl_vlw_end");
#endif
// Labels, titles and buttons use the same typeface as the digits (Oswald Bold) in two sizes, made by
// tools/make_vlw_text.py: t_big (18 px capitals) and t_small (12 px). If they fail to load, the library's plain
// bitmap fonts are used instead.
lgfx::VLWfont textFont[2];
lgfx::PointerWrapper textData[2];
bool textFontsOk = false;
lgfx::VLWfont rpmFont;                 // 80 px digits for the RPM view (fonts/d_rpm.vlw)
lgfx::PointerWrapper rpmData;
bool rpmFontOk = false;
lgfx::VLWfont midFont, xlFont;         // values on the tool screens (t_mid, 15 px capitals) and thread names (t_xl, 33 px)
lgfx::PointerWrapper midData, xlData;
bool midFontOk = false, xlFontOk = false;
#define FONT_M (midFontOk ? (const lgfx::IFont *)&midFont : (const lgfx::IFont *)&fonts::Font2)
#define FONT_XL (xlFontOk ? (const lgfx::IFont *)&xlFont : (const lgfx::IFont *)&fonts::Font4)
#define FONT_L (textFontsOk ? (const lgfx::IFont *)&textFont[0] : (const lgfx::IFont *)&fonts::Font4)
#define FONT_S (textFontsOk ? (const lgfx::IFont *)&textFont[1] : (const lgfx::IFont *)&fonts::Font2)
lgfx::VLWfont digitFont[3];
lgfx::PointerWrapper digitData[3];
bool digitFontsOk = false;
void loadDigitFonts() {
  const uint8_t *s[3] = {vlw_big_start, vlw_mid_start, vlw_small_start}, *e[3] = {vlw_big_end, vlw_mid_end, vlw_small_end};
  digitFontsOk = true;
  for (int i = 0; i < 3; i++) { digitData[i].set(s[i], e[i] - s[i]); if (!digitFont[i].loadFont(&digitData[i])) digitFontsOk = false; }
  Serial.printf("display: digit fonts %s\n", digitFontsOk ? "loaded" : "failed, using the bitmap font");
  const uint8_t *ts[2] = {vlw_tbig_start, vlw_tsmall_start}, *te[2] = {vlw_tbig_end, vlw_tsmall_end};
  bool ok = true;
  for (int i = 0; i < 2; i++) { textData[i].set(ts[i], te[i] - ts[i]); if (!textFont[i].loadFont(&textData[i])) ok = false; }
  textFontsOk = ok;
  rpmData.set(vlw_rpm_start, vlw_rpm_end - vlw_rpm_start); rpmFontOk = rpmFont.loadFont(&rpmData);
  midData.set(vlw_tmid_start, vlw_tmid_end - vlw_tmid_start); midFontOk = midFont.loadFont(&midData);
  xlData.set(vlw_txl_start, vlw_txl_end - vlw_txl_start); xlFontOk = xlFont.loadFont(&xlData);
}
// Draws a number as large as fits: the biggest smooth font no taller than maxH whose width fits maxW. The text
// colour is set without a background colour on purpose: the library then reads the pixels already in the frame
// buffer and blends the anti-aliased edges onto them, so the digits sit on any background with no box round
// them. (With a background colour it paints boxes instead.) Falls back to the scaled bitmap font if the smooth
// fonts did not load.
void drawNumber(const char *txt, int x, int y, int maxW, int maxH, textdatum_t datum, uint32_t col) {
  g->setTextDatum(datum); g->setTextColor(col);
  if (digitFontsOk) {
    int pick = 2;
    for (int i = 0; i < 3; i++) { g->setFont(&digitFont[i]); if (g->fontHeight() <= maxH && g->textWidth(txt) <= maxW) { pick = i; break; } }
    g->setFont(&digitFont[pick]); g->drawString(txt, x, y);
    return;
  }
  g->setFont(&fonts::Font7); g->setTextSize(1);
  float sc = min((float)maxH / 48.0f, (float)maxW / max(1, (int)g->textWidth(txt)));
  g->setTextSize(sc); g->setTextColor(col); g->drawString(txt, x, y); g->setTextSize(1);
}
void drawBigNumber(const char *txt, int right, int y, int maxW, uint32_t col) {
  drawNumber(txt, right, y + DISP_ROW_H / 2, maxW, DISP_ROW_H - 8, middle_right, col);
}
// The sub label (DIA, LONG, FWD) is near white: it stays readable whatever the panel's colour order.
void drawRowLabel(int y, const char *name, const char *sub, uint32_t col) {
  if (!artActive) {
    g->fillRoundRect(2, y + 2, 316, DISP_ROW_H - 4, 6, theme().panel); g->fillRect(2, y + 2, 4, DISP_ROW_H - 4, col);
    if (theme().line) g->drawRoundRect(2, y + 2, 316, DISP_ROW_H - 4, 6, theme().line);
  }
  g->setTextDatum(top_left); g->setFont(FONT_L); g->setTextColor(col); g->drawString(name, 11, y + 12);
  g->setFont(FONT_S); g->setTextColor(0xe6edf7); g->drawString(sub, 11, y + 52);
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
    lcd.fillScreen(theme().bg);
    lcd.setTextDatum(middle_center); lcd.setTextColor(0xe6edf7); lcd.setFont(FONT_L);
    lcd.drawString("Touch calibration", 160, 108);
    char m[32]; snprintf(m, sizeof m, "Press the cross, %d of 3", i + 1);
    lcd.setFont(FONT_S); lcd.drawString(m, 160, 138);
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
  lcd.fillScreen(theme().bg); lcd.setFont(FONT_L); lcd.setTextDatum(middle_center); lcd.setTextColor(ok ? 0x22c55e : 0xf59e0b);
  lcd.drawString(ok ? "Calibrated" : "No touch detected", 160, 120);
  vTaskDelay(pdMS_TO_TICKS(900));
  lcd.setRotation(flip ? 3 : 1);
}

// ---------------- on-screen menu ----------------
// A tap anywhere on the DRO opens it. Buttons are finger sized; nothing here needs the web page.
struct Btn { int x, y, w, h; };
// Settings menu: a title bar with Close, then a 3 x 3 grid of buttons.
const Btn MENU_CLOSE = {236, 3, 78, 28}, MENU_TOOLS = {158, 3, 74, 28};
const Btn MB[9] = {{6, 36, 100, 63}, {110, 36, 100, 63}, {214, 36, 100, 63},
                   {6, 103, 100, 63}, {110, 103, 100, 63}, {214, 103, 100, 63},
                   {6, 170, 100, 63}, {110, 170, 100, 63}, {214, 170, 100, 63}};
enum { B_RPM, B_VIEW, B_SWAP, B_ZERO_X, B_ZERO_Z, B_XDIA, B_FLIP, B_CAL, B_INFO };
const Btn YES_BTN = {6, 150, 152, 48}, NO_BTN = {162, 150, 152, 48};
const Btn INFO_BACK = {6, 186, 152, 48}, INFO_REBOOT = {162, 186, 152, 48};
// View screen: what fills the DRO. Five layouts and Back.
const Btn VB[6] = {{6, 6, 152, 52}, {162, 6, 152, 52}, {6, 64, 152, 52}, {162, 64, 152, 52}, {6, 122, 152, 52}, {162, 122, 152, 52}};
const char *VIEW_NAME[5] = {"Everything", "RPM only", "X only", "Z only", "X and Z"};
// Speed keypad: the number being typed on top, then four rows of four keys.
const char *KP_LBL[16] = {"7", "8", "9", "Clear", "4", "5", "6", "Knob", "1", "2", "3", "Cancel", "0", "00", "Del", "OK"};
Btn kpBtn(int i) { return {6 + (i % 4) * 78, 64 + (i / 4) * 44, 74, 40}; }
bool hit(const Btn &b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }
// A button: filled, with a one-pixel lighter edge so it reads as raised on either background. With two lines the
// second is the current value, in amber.
uint32_t edgeOf(uint32_t fill) { return ((fill >> 16 & 0xff) + 34 > 255 ? 255 : (fill >> 16 & 0xff) + 34) << 16 | ((fill >> 8 & 0xff) + 38 > 255 ? 255 : (fill >> 8 & 0xff) + 38) << 8 | ((fill & 0xff) + 46 > 255 ? 255 : (fill & 0xff) + 46); }
void drawBtn(const Btn &b, const char *l1, const char *l2, uint32_t fill = theme().btn, uint32_t txt = 0xe6edf7, bool small = false) {
  uint32_t edge = edgeOf(fill);
  if (artActive && fill == theme().btn) { fill = S.dispBlack ? 0x0a0d12 : 0x0a1c38; edge = 0x2e84e6; }
  g->fillRoundRect(b.x, b.y, b.w, b.h, 8, fill); g->drawRoundRect(b.x, b.y, b.w, b.h, 8, edge);
  g->setTextDatum(middle_center); g->setTextColor(txt);
  if (l2) {
    g->setFont(FONT_L); g->drawString(l1, b.x + b.w / 2, b.y + b.h / 2 - 10);
    g->setFont(FONT_S); g->setTextColor(0xfbbf24); g->drawString(l2, b.x + b.w / 2, b.y + b.h / 2 + 13);
  } else { g->setFont(small ? FONT_S : FONT_L); g->drawString(l1, b.x + b.w / 2, b.y + b.h / 2); }
}
// Title bar used by the menu screens.
void drawTitle(const char *title) {
  g->fillRect(0, 0, 320, 34, theme().panel); g->drawFastHLine(0, 34, 320, theme().line ? theme().line : 0x223052);
  g->fillRect(0, 0, 4, 34, 0x38bdf8);
  g->setFont(FONT_L); g->setTextDatum(middle_left); g->setTextColor(0xe6edf7); g->drawString(title, 12, 17);
}
void drawMenu(const DispSnap &s) {
  if (artBg(ART_MENU)) {                                  // artwork has the buttons and labels: add the current values
    char v[16]; if (s.preset > 0) snprintf(v, sizeof v, "%d rpm", s.preset); else snprintf(v, sizeof v, "by knob");
    const char *shortView[5] = {"all", "RPM only", "X only", "Z only", "X and Z"};
    const char *val[9] = {v, shortView[S.dispView], S.dispBlack ? "black" : "dark blue", nullptr, nullptr, S.xDia ? "diameter" : "radius", S.dispFlip ? "flipped" : "normal", "calibrate", "reboot"};
    g->setFont(FONT_S); g->setTextDatum(middle_left); g->setTextColor(0xfbbf24);
    for (int i = 0; i < 9; i++) if (val[i]) { g->setClipRect(MB[i].x + 38, MB[i].y + 32, 60, 24); g->drawString(val[i], MB[i].x + 38, MB[i].y + 43); }
    g->clearClipRect();
    return;
  }
  g->fillScreen(theme().bg);
  drawTitle("Settings");
  drawBtn(MENU_CLOSE, "Close", nullptr, theme().btn, 0xe6edf7, true);
  drawBtn(MENU_TOOLS, "Tools", nullptr, 0x14532d, 0xe6edf7, true);
  char v[16]; if (s.preset > 0) snprintf(v, sizeof v, "%d rpm", s.preset); else snprintf(v, sizeof v, "by knob");
  drawBtn(MB[B_RPM], "Set RPM", v, 0x14532d);
  drawBtn(MB[B_VIEW], "View", VIEW_NAME[S.dispView]);
  drawBtn(MB[B_SWAP], "Colours", S.dispBlack ? "black" : "dark blue");
  drawBtn(MB[B_ZERO_X], "Zero X", nullptr, 0x0c4a6e);
  drawBtn(MB[B_ZERO_Z], "Zero Z", nullptr, 0x0c4a6e);
  drawBtn(MB[B_XDIA], "X shows", S.xDia ? "diameter" : "radius");
  drawBtn(MB[B_FLIP], "Screen", S.dispFlip ? "upside down" : "normal");
  drawBtn(MB[B_CAL], "Touch", "calibrate");
  drawBtn(MB[B_INFO], "Info", "and reboot");
}
// Speed keypad. kp is the text typed so far.
void drawKeypad(const DispSnap &s, const char *kp) {
  if (artBg(ART_KEYPAD)) {
    char hint[40]; snprintf(hint, sizeof hint, "%d to %d. Knob starts it.", s.minRpm, s.maxRpm);
    g->setFont(FONT_S); g->setTextDatum(middle_left); g->setTextColor(0xfbbf24); g->drawString(hint, 18, 44);
    g->setClipRect(200, 6, 110, 52); drawNumber(kp[0] ? kp : "0", 306, 32, 104, 52, middle_right, kp[0] ? 0x22e664 : 0x3b5578); g->clearClipRect();
    return;
  }
  g->fillScreen(theme().bg);
  g->fillRoundRect(6, 4, 308, 56, 8, theme().panel); g->drawRoundRect(6, 4, 308, 56, 8, theme().line ? theme().line : 0x223052);
  g->setFont(FONT_L); g->setTextDatum(top_left); g->setTextColor(0x22c55e); g->drawString("SET RPM", 16, 9);
  char hint[40]; snprintf(hint, sizeof hint, "%d to %d. Knob starts it.", s.minRpm, s.maxRpm);
  g->setFont(FONT_S); g->setTextColor(0x8fa1c2); g->drawString(hint, 16, 37);
  g->setClipRect(180, 5, 130, 54); drawNumber(kp[0] ? kp : "0", 306, 32, 124, 52, middle_right, kp[0] ? 0x22c55e : 0x475569); g->clearClipRect();
  for (int i = 0; i < 16; i++) {
    bool word = strlen(KP_LBL[i]) > 2;
    uint32_t fill = i == 15 ? 0x166534 : i == 7 ? 0x0c4a6e : theme().btn;
    drawBtn(kpBtn(i), KP_LBL[i], nullptr, fill, 0xe6edf7, word && i != 15);
  }
}
// Colours screen: the red/blue swap with its check squares, and the background choice.
const Btn CB[3] = {{6, 6, 152, 80}, {162, 6, 152, 80}, {6, 186, 308, 48}};
void drawColorsMenu(const DispSnap &s) {
  clearScreen();
  drawBtn(CB[0], "Swap red / blue", S.dispSwapRb ? "on" : "off");
  const uint32_t rgb[3] = {0xff0000, 0x00ff00, 0x0000ff}; const char *ltr[3] = {"R", "G", "B"};
  g->setFont(&fonts::Font0); g->setTextDatum(middle_center);
  for (int i = 0; i < 3; i++) {
    int bx = CB[0].x + CB[0].w / 2 - 25 + 18 * i, by = CB[0].y + CB[0].h - 22;
    g->fillRoundRect(bx, by, 14, 14, 3, rgb[i]); g->setTextColor(0xffffff); g->drawString(ltr[i], bx + 7, by + 7);
  }
  drawBtn(CB[1], "Background", S.dispBlack ? "black" : "dark blue");
  g->setFont(FONT_S); g->setTextDatum(top_left); g->setTextColor(0x8fa1c2);
  g->drawString("The squares should read red, green, blue.", 8, 100);
  g->drawString("If they don't, turn the swap on.", 8, 120);
  g->drawString("Black background: everything on black, with a", 8, 148);
  g->drawString("thin outline round each row.", 8, 168);
  drawBtn(CB[2], "Back", nullptr);
}
void drawViewMenu(const DispSnap &s) {
  if (artBg(ART_VIEW)) {                                  // artwork has the six buttons: ring the one in use
    const Btn &b = VB[S.dispView];
    g->drawRoundRect(b.x, b.y, b.w, b.h, 8, 0x28eb6e); g->drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 7, 0x28eb6e);
    return;
  }
  g->fillScreen(theme().bg);
  for (int i = 0; i < 5; i++) drawBtn(VB[i], VIEW_NAME[i], nullptr, i == S.dispView ? 0x0c4a6e : theme().btn);
  drawBtn(VB[5], "Back", nullptr);
  g->setFont(FONT_S); g->setTextDatum(top_left); g->setTextColor(0x8fa1c2);
  g->drawString("Pick what fills the screen. Tap it to switch.", 8, 188);
  g->drawString("No axis encoders fitted yet? Choose RPM only.", 8, 208);
}
void drawConfirmReboot(const DispSnap &s) {
  clearScreen();
  drawTitle("Reboot");
  g->setTextDatum(middle_center); g->setFont(FONT_L); g->setTextColor(0xe6edf7);
  g->drawString("Restart the controller now?", 160, 80);
  g->setFont(FONT_S); g->setTextColor(0x8fa1c2); g->drawString("The web page reconnects by itself.", 160, 110);
  drawBtn(YES_BTN, "Yes, reboot", nullptr, 0x7f1d1d);
  drawBtn(NO_BTN, "No", nullptr);
}
// Firmware, network and the run-hour counters (spindle time counts whenever it turns above 10 rpm, and is
// saved every minute, so it is the spindle's total mileage).
void drawInfo(const DispSnap &s) {
  if (artBg(ART_INFO)) {
    g->setFont(FONT_S); g->setTextDatum(middle_left);
    int y = 54; char v[64];
    auto line = [&](const char *k, const char *val) {
      g->setTextColor(0x8fa1c2); g->drawString(k, 14, y); g->setTextColor(0xe6edf7); g->drawString(val, 120, y); y += 22;
    };
    line("Firmware", "v" FW_VERSION);
    if (s.up) snprintf(v, sizeof v, "%s   %d dBm", s.ip, s.rssi); else if (s.ap) snprintf(v, sizeof v, "own network, %s", s.ip); else snprintf(v, sizeof v, "not connected");
    line("Wi-Fi", v);
    snprintf(v, sizeof v, "http://%s.local", s.host); line("Address", v);
    line("CNC", s.cnc ? "online" : "offline");
    snprintf(v, sizeof v, "%lu h %02lu min", (unsigned long)(s.spinS / 3600), (unsigned long)(s.spinS % 3600 / 60)); line("Spindle run", v);
    snprintf(v, sizeof v, "%lu h %02lu min", (unsigned long)(s.powS / 3600), (unsigned long)(s.powS % 3600 / 60)); line("Powered on", v);
    return;
  }
  g->fillScreen(theme().bg);
  drawTitle("ESP32 Lathe");
  g->setFont(FONT_S); g->setTextDatum(top_left);
  int y = 44; char v[64];
  auto line = [&](const char *k, const char *val) {
    g->setTextColor(0x8fa1c2); g->drawString(k, 12, y); g->setTextColor(0xe6edf7); g->drawString(val, 118, y); y += 23;
  };
  line("Firmware", "v" FW_VERSION);
  if (s.up) snprintf(v, sizeof v, "%s   %d dBm", s.ip, s.rssi); else if (s.ap) snprintf(v, sizeof v, "own network, %s", s.ip); else snprintf(v, sizeof v, "not connected");
  line("Wi-Fi", v);
  snprintf(v, sizeof v, "http://%s.local", s.host); line("Address", v);
  line("CNC", s.cnc ? "online" : "offline");
  snprintf(v, sizeof v, "%lu h %02lu min", (unsigned long)(s.spinS / 3600), (unsigned long)(s.spinS % 3600 / 60)); line("Spindle run", v);
  snprintf(v, sizeof v, "%lu h %02lu min", (unsigned long)(s.powS / 3600), (unsigned long)(s.powS % 3600 / 60)); line("Powered on", v);
  drawBtn(INFO_BACK, "Back", nullptr);
  drawBtn(INFO_REBOOT, "Reboot", nullptr, 0x7f1d1d);
}
// ---------------- single-value views ----------------
// A number in the seven-segment font, as large as maxW and maxScale allow, centred on (cx, cy).
void bigCentered(const char *txt, int cx, int cy, int maxW, float maxScale, uint32_t col) {
  drawNumber(txt, cx, cy, maxW, (int)(48 * maxScale), middle_center, col);
}
void menuMark(int x, int y) { for (int i = 0; i < 3; i++) g->fillRect(x, y + 4 * i, 14, 2, 0x8fa1c2); }
const char *toolText(const DispSnap &s) { const char *t = strstr(s.tool, " + "); return t ? t + 3 : (s.tool[0] ? s.tool : "No tool"); }
// Bottom line of the single-value views: CNC and Wi-Fi state.
void footerStatus(const DispSnap &s, int y) {
  char f[64]; snprintf(f, sizeof f, "%s    %s", s.cnc ? "CNC online" : "CNC offline", s.up ? s.ip : (s.ap ? "AP 192.168.4.1" : "no Wi-Fi"));
  g->setFont(FONT_S); g->setTextDatum(top_left); g->setTextColor(0x8fa1c2); g->drawString(f, 8, y);
}
// The spindle graph: the last 37 s, newest at the right. Green is the measured speed, drawn in a window of
// +/-20 % round the target (dotted line) so a dip under load and the recovery are easy to see. Amber is the drive
// the controller is giving, 0 to 100 % of the graph height: when a cut loads the spindle, amber climbs while
// green comes back to the dotted line.
// Plots into the area (px0, py0, pw, ph). Returns the speed at the bottom and top edge through lo and hi.
void plotSpindle(int px0, int py0, int pw, int ph, const DispSnap &s, float &lo, float &hi) {
  const int n = histCount, head = histHead;
  lo = 0; hi = s.maxRpm > 0 ? s.maxRpm : 2000;
  if (s.target > 0) { float span = max(60.0f, s.target * 0.2f); lo = s.target - span; hi = s.target + span; }
  if (s.target > 0) for (int i = px0; i < px0 + pw; i += 6) g->drawFastHLine(i, py0 + ph / 2, 3, 0x94a3b8);                       // the target
  int lx = 0, lyr = 0, lyl = 0;
  for (int i = 0; i < n; i++) {
    int idx = (head - n + i + 2 * HIST_N) % HIST_N;
    int xx = px0 + (pw - 1) * (HIST_N - n + i) / (HIST_N - 1);
    float r = constrain((float)histRpm[idx], lo, hi);
    int yr = py0 + ph - 1 - (int)((r - lo) / (hi - lo) * (ph - 1));
    int yl = py0 + ph - 1 - histLoad[idx] * (ph - 1) / 100;
    if (i) {
      g->drawLine(lx, lyl, xx, yl, 0xf59e0b);
      g->drawLine(lx, lyr, xx, yr, 0x22e664); g->drawLine(lx, lyr + 1, xx, yr + 1, 0x22e664);
    }
    lx = xx; lyr = yr; lyl = yl;
  }
}
void drawSpindleGraph(int x, int y, int w, int h, const DispSnap &s) {
  g->fillRoundRect(x, y, w, h, 5, theme().panel); g->drawRoundRect(x, y, w, h, 5, theme().line ? theme().line : 0x223052);
  const int px0 = x + 4, pw = w - 8, py0 = y + 4, ph = h - 8;
  for (int i = 1; i < 4; i++) if (i != 2 || s.target <= 0) g->drawFastHLine(px0, py0 + ph * i / 4, pw, theme().line ? 0x1c1c1c : 0x18233d);
  float lo, hi; plotSpindle(px0, py0, pw, ph, s, lo, hi);
  g->setFont(&fonts::Font0); g->setTextColor(0x8fa1c2);
  char b[12];
  if (s.target > 0) {
    g->setTextDatum(top_left); snprintf(b, sizeof b, "%d", (int)hi); g->drawString(b, px0 + 1, py0);
    g->setTextDatum(bottom_left); snprintf(b, sizeof b, "%d", (int)lo); g->drawString(b, px0 + 1, py0 + ph);
  }
}
// RPM view on its artwork (art/rpm.png): the panels, icons, SPINDLE, RPM and LOAD labels are in the picture.
void drawRpmArt(const DispSnap &s) {
  uint32_t col = s.enc || s.rpm < 1 ? 0x22e664 : 0xf59e0b;
  g->fillRoundRect(126, 9, 62, 22, 11, s.fwd ? 0x105c2c : 0x7c2d12); g->drawRoundRect(126, 9, 62, 22, 11, s.fwd ? 0x28eb6e : 0xfb923c);
  g->setFont(FONT_S); g->setTextDatum(middle_center); g->setTextColor(0xe6edf7); g->drawString(s.fwd ? "FWD" : "REV", 157, 20);
  char tg[16]; snprintf(tg, sizeof tg, "%d", s.target);
  g->setFont(FONT_L); g->setTextDatum(middle_right); g->setTextColor(0xe6edf7); g->drawString(tg, 308, 20);
  int tw = g->textWidth(tg);
  g->setFont(FONT_S); g->setTextColor(s.preset > 0 ? 0xfbbf24 : 0x8fa1c2); g->drawString(s.preset > 0 ? "PRESET" : "SET", 308 - tw - 7, 21);
  char r[8]; snprintf(r, sizeof r, "%d", (int)(s.rpm + 0.5f));
  if (rpmFontOk) { g->setFont(&rpmFont); g->setTextDatum(middle_center); g->setTextColor(col); g->drawString(r, 160, 81); }
  else drawNumber(r, 160, 81, 300, 72, middle_center, col);
  float lo, hi; plotSpindle(34, 148, 276, 52, s, lo, hi);
  if (s.target > 0) {
    char b[12]; g->setFont(FONT_S); g->setTextColor(0x8fa1c2); g->setTextDatum(middle_left);
    snprintf(b, sizeof b, "%d", (int)hi); g->drawString(b, 9, 154); snprintf(b, sizeof b, "%d", (int)lo); g->drawString(b, 9, 196);
  }
  g->setFont(FONT_S); g->setTextDatum(middle_left); g->setClipRect(30, 212, 146, 22);
  if (s.alarms) { g->setTextColor(0xfb923c); g->drawString(s.alarm, 30, 223); } else { g->setTextColor(0xfbbf24); g->drawString(toolText(s), 30, 223); }
  g->clearClipRect();
  char ld[8]; snprintf(ld, sizeof ld, "%d%%", s.load);
  g->setFont(FONT_L); g->setTextDatum(middle_right); g->setTextColor(0xf59e0b); g->drawString(ld, 270, 223);
}
// RPM only: a title strip, the speed in 120 px digits, the graph, and one line with the tool (or alarm) and drive.
// A tap on the title strip opens the speed keypad; a tap anywhere else opens Settings.
const int RPM_STRIP_H = 30;
void drawRpmView(const DispSnap &s) {
  if (artBg(ART_RPM)) { drawRpmArt(s); return; }
  g->fillScreen(theme().bg);
  uint32_t col = s.enc || s.rpm < 1 ? 0x22c55e : 0xf59e0b;
  g->fillRect(0, 0, 320, RPM_STRIP_H, theme().panel); g->drawFastHLine(0, RPM_STRIP_H, 320, theme().line ? theme().line : 0x223052);
  g->fillRect(0, 0, 4, RPM_STRIP_H, 0x22c55e);
  g->setFont(FONT_L); g->setTextDatum(middle_left); g->setTextColor(0x22c55e); g->drawString("SPINDLE", 12, 15);
  int bx = 12 + g->textWidth("SPINDLE") + 10;
  g->fillRoundRect(bx, 5, 42, 20, 5, s.fwd ? 0x14532d : 0x7c2d12);
  g->setFont(FONT_S); g->setTextDatum(middle_center); g->setTextColor(0xe6edf7); g->drawString(s.fwd ? "FWD" : "REV", bx + 21, 15);
  char tg[16]; snprintf(tg, sizeof tg, "%d", s.target);
  g->setFont(FONT_L); g->setTextDatum(middle_right); g->setTextColor(0xe6edf7); g->drawString(tg, 312, 15);
  int tw = g->textWidth(tg);
  g->setFont(FONT_S); g->setTextColor(s.preset > 0 ? 0xfbbf24 : 0x8fa1c2); g->drawString(s.preset > 0 ? "PRESET" : "SET", 312 - tw - 7, 16);
  char r[8]; snprintf(r, sizeof r, "%d", (int)(s.rpm + 0.5f));
  bigCentered(r, 160, 92, 300, 2.5f, col);
  drawSpindleGraph(4, 154, 312, 66, s);
  g->setFont(FONT_S); g->setTextDatum(middle_left); g->setClipRect(6, 222, 196, 18);
  if (s.alarms) { g->fillRoundRect(6, 222, 196, 18, 4, 0xb45309); g->setTextColor(0xffffff); g->drawString(s.alarm, 10, 231); }
  else { g->setTextColor(0xfbbf24); g->drawString(toolText(s), 8, 231); }
  g->clearClipRect();
  char ld[16]; snprintf(ld, sizeof ld, "LOAD %d%%", s.load);
  g->setTextDatum(middle_right); g->setTextColor(0xf59e0b); g->drawString(ld, 292, 231);
  menuMark(300, 226);
}
// X only or Z only: one axis in digits up to 120 px tall, the other axis and the spindle small underneath.
void drawAxisView(const DispSnap &s, bool isX) {
  clearScreen();
  g->setFont(FONT_L); g->setTextDatum(top_left); g->setTextColor(0x38bdf8); g->drawString(isX ? "X" : "Z", 8, 6);
  g->setTextColor(0xe6edf7); g->drawString(isX ? (s.xDia ? "DIAMETER" : "RADIUS") : "LONGITUDINAL", 40, 6);
  g->setTextDatum(top_right); g->setTextColor(0x8fa1c2); g->drawString("inch", 312, 6);
  char b[16]; snprintf(b, sizeof b, "%.4f", isX ? s.x : s.z);
  bigCentered(b, 160, 96, 304, 2.5f, 0x38bdf8);
  g->setFont(FONT_L); g->setTextDatum(top_left); g->setTextColor(0x8fa1c2);
  char o[32]; snprintf(o, sizeof o, "%s %.4f", isX ? "Z" : "X", isX ? s.z : s.x); g->drawString(o, 8, 166);
  snprintf(o, sizeof o, "S %d %s", (int)(s.rpm + 0.5f), s.fwd ? "FWD" : "REV"); g->setTextDatum(top_right); g->drawString(o, 312, 166);
  g->setFont(FONT_S); g->setTextDatum(top_left); g->setClipRect(8, 194, 304, 20);
  if (s.alarms) { g->setTextColor(0xf59e0b); g->drawString(s.alarm, 8, 194); } else { g->setTextColor(0xfbbf24); g->drawString(toolText(s), 8, 194); }
  g->clearClipRect();
  footerStatus(s, 216); menuMark(300, 222);
}
// X and Z: two rows of 118 px, no spindle.
void drawXZView(const DispSnap &s) {
  bool onArt = artBg(ART_XZ); if (!onArt) g->fillScreen(theme().bg);
  const int rh = 118;
  for (int i = 0; i < 2; i++) {
    int y = i * (rh + 4);
    if (!onArt) {
      g->fillRoundRect(2, y + 2, 316, rh - 4, 6, theme().panel); g->fillRect(2, y + 2, 4, rh - 4, 0x38bdf8);
      if (theme().line) g->drawRoundRect(2, y + 2, 316, rh - 4, 6, theme().line);
    }
    g->setTextDatum(top_left); g->setFont(FONT_L); g->setTextColor(0x38bdf8); g->drawString(i ? "Z" : "X", 11, y + 12);
    g->setFont(FONT_S); g->setTextColor(0xe6edf7); g->drawString(i ? "LONG" : (s.xDia ? "DIA" : "RAD"), 11, y + 44);
    char b[16]; snprintf(b, sizeof b, "%.4f", i ? s.z : s.x);
    drawNumber(b, 314, y + rh / 2, 270, rh - 12, middle_right, 0x38bdf8);
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
  clearScreen(ART_MAIN);
  // DRO rows
  drawAxis(0, "X", s.xDia ? "DIA" : "RAD", s.x);
  drawAxis(DISP_ROW_H, "Z", "LONG", s.z);
  for (int i = 0; i < 3; i++) g->fillRect(11, 69 + 4 * i, 14, 2, 0x8fa1c2);    // menu mark: a tap opens Settings
  // Spindle row: big RPM on the left, target, speed bar and tool (or alarm) in a narrow column on the right
  const int y = 2 * DISP_ROW_H, colX = 216, colW = 96;
  drawRowLabel(y, "S", s.fwd ? "FWD" : "REV", 0x22c55e);
  char r[8]; snprintf(r, sizeof r, "%d", (int)(s.rpm + 0.5f));
  drawBigNumber(r, colX - 6, y, colX - 6 - DISP_LABEL_W - 4, 0x22c55e);
  g->setFont(FONT_S); g->setTextDatum(top_left); g->setTextColor(0xe6edf7);
  char tg[16]; snprintf(tg, sizeof tg, "SET %d", s.target); g->drawString(tg, colX, y + 10);
  int mx = s.maxRpm > 0 ? s.maxRpm : 2000;
  g->fillRoundRect(colX, y + 32, colW, 10, 3, theme().btn);
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
  lcd.fillScreen(theme().bg);
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
  lcd.setTextDatum(middle_center); lcd.setTextColor(0xe6edf7); lcd.setFont(FONT_L); lcd.drawString("ESP32 Lathe", 160, 40);
  lcd.setFont(FONT_S); lcd.setTextColor(0x8fa1c2); lcd.drawString("firmware v" FW_VERSION, 160, 66);
}
// Draws a whole frame off screen, then sends it in one go, so nothing flickers.
// ---------------- tool screens: Thread Assist, Spindle Diagnostics, Material & Speed ----------------
// Settings > TOOLS opens a list of the three. Like the other screens, each has its artwork (art/thread.png,
// diag.png, mat.png, numpad.png, tools.png, pick.png) with the live values painted on top, and a plain version.
const Btn TB[4] = {{4, 36, 312, 52}, {4, 92, 312, 52}, {4, 148, 312, 52}, {4, 206, 312, 30}};
struct ThreadPreset { const char *size, *pitch; float majorMm, pitchMm; };
const ThreadPreset TP[2][12] = {
  {{"M3", "0.5", 3, 0.5f}, {"M4", "0.7", 4, 0.7f}, {"M5", "0.8", 5, 0.8f}, {"M6", "1.0", 6, 1.0f}, {"M8", "1.25", 8, 1.25f}, {"M8", "1.0", 8, 1.0f},
   {"M10", "1.5", 10, 1.5f}, {"M10", "1.25", 10, 1.25f}, {"M12", "1.75", 12, 1.75f}, {"M12", "1.25", 12, 1.25f}, {"M14", "2.0", 14, 2.0f}, {"M16", "2.0", 16, 2.0f}},
  {{"#10", "32", 4.826f, 25.4f / 32}, {"1/4", "20", 6.35f, 25.4f / 20}, {"1/4", "28", 6.35f, 25.4f / 28}, {"5/16", "18", 7.9375f, 25.4f / 18},
   {"5/16", "24", 7.9375f, 25.4f / 24}, {"3/8", "16", 9.525f, 25.4f / 16}, {"3/8", "24", 9.525f, 25.4f / 24}, {"7/16", "14", 11.1125f, 25.4f / 14},
   {"1/2", "13", 12.7f, 25.4f / 13}, {"1/2", "20", 12.7f, 25.4f / 20}, {"5/8", "11", 15.875f, 25.4f / 11}, {"3/4", "10", 19.05f, 25.4f / 10}}};
const int PASS_OPT[4] = {6, 8, 11, 14};
struct ThreadUi { int sys, idx, passSel; float len; uint32_t sent; };
ThreadUi TA = {0, 7, 2, 0.75f, 0};                     // M10 x 1.25, 11 passes, 0.75 in long
int pickSys = 0;                                       // page of the preset screen: 0 metric, 1 inch
struct MatUi { int mat, tool, cut; float dia; };
MatUi MS = {1, 1, 1, 1.0f};                            // steel, carbide, finish, 1 in
char np[8] = ""; int npFor = 0;                        // number pad text; what it is for: 0 thread length, 1 work diameter

void titleRight(const char *lab, const char *val) {
  g->setFont(FONT_L); g->setTextDatum(middle_right); g->setTextColor(0xe6edf7); g->drawString(val, 310, 17);
  int tw = g->textWidth(val); g->setFont(FONT_S); g->setTextColor(0xfbbf24); g->drawString(lab, 310 - tw - 6, 18);
}
void smallLabel(const char *t, int x, int y, textdatum_t d = middle_left) { g->setFont(&fonts::Font0); g->setTextDatum(d); g->setTextColor(0x8fa1c2); g->drawString(t, x, y); }
void plainPanel(int x, int y, int w, int h) { g->fillRoundRect(x, y, w, h, 6, theme().panel); g->drawRoundRect(x, y, w, h, 6, theme().line ? theme().line : 0x223052); }
void ring(const Btn &b) { g->drawRoundRect(b.x, b.y, b.w, b.h, 6, 0x28eb6e); g->drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 5, 0x28eb6e); }
void val(const char *t, int x, int y, const lgfx::IFont *f, uint32_t col, textdatum_t d = middle_left) { g->setFont(f); g->setTextDatum(d); g->setTextColor(col); g->drawString(t, x, y); }

void drawTools(const DispSnap &s) {
  if (artBg(ART_TOOLS)) return;
  g->fillScreen(theme().bg); drawTitle("Tools");
  drawBtn(TB[0], "Thread Assist", "pick a thread, send the program"); drawBtn(TB[1], "Spindle Diagnostics", "speed hold and sensor health");
  drawBtn(TB[2], "Material & Speed", "suggested RPM"); drawBtn(TB[3], "Back", nullptr, theme().btn, 0xe6edf7, true);
}

// ---- Thread Assist
const Btn TH_BTN[4] = {{4, 206, 76, 30}, {82, 206, 76, 30}, {160, 206, 76, 30}, {238, 206, 78, 30}};
const Btn TH_NAME = {4, 35, 312, 48}, TH_LEN = {160, 87, 78, 36}, TH_PASS = {238, 87, 78, 36};
void threadName(char *b, size_t n) { const ThreadPreset &t = TP[TA.sys][TA.idx]; snprintf(b, n, TA.sys ? "%s-%s" : "%s x %s", t.size, t.pitch); }
void threadSend() {
  const ThreadPreset &t = TP[TA.sys][TA.idx]; char name[24]; threadName(name, sizeof name);
  jobSet(name, threadProgram(name, t.pitchMm, t.majorMm, TA.len, PASS_OPT[TA.passSel], 200));
  TA.sent = jobId;
}
void drawThread(const DispSnap &s) {
  if (!artBg(ART_THREAD)) {
    g->fillScreen(theme().bg); drawTitle("Thread Assist");
    plainPanel(4, 35, 312, 48); plainPanel(4, 87, 312, 36); plainPanel(4, 127, 100, 60); plainPanel(108, 127, 208, 60);
    const char *l[4] = {"MAJOR DIA", "PITCH", "LENGTH", "PASSES"}; for (int i = 0; i < 4; i++) smallLabel(l[i], 10 + i * 78, 95);
    smallLabel("THREAD DEPTH", 10, 135); smallLabel("CUT DEPTH AFTER EACH PASS", 114, 135);
    const char *bl[4] = {"Presets", "Passes", "Send", "Back"};
    for (int i = 0; i < 4; i++) drawBtn(TH_BTN[i], bl[i], nullptr, i == 2 ? 0x166534 : theme().btn, 0xe6edf7, true);
  }
  const ThreadPreset &t = TP[TA.sys][TA.idx]; char b[48];
  snprintf(b, sizeof b, "%d", (int)(s.rpm + 0.5f)); titleRight("RPM", b);
  threadName(b, sizeof b); val(b, 186, 59, FONT_XL, 0x22e664, middle_center);
  snprintf(b, sizeof b, "%.4f\"", t.majorMm / 25.4f); val(b, 10, 110, FONT_M, 0xfbbf24);
  snprintf(b, sizeof b, TA.sys ? "%s TPI" : "%s mm", t.pitch); val(b, 88, 110, FONT_M, 0xfbbf24);
  snprintf(b, sizeof b, "%.3f\"", TA.len); val(b, 166, 110, FONT_M, 0xfbbf24);
  const int n = PASS_OPT[TA.passSel];
  snprintf(b, sizeof b, "%d + 2", n); val(b, 244, 110, FONT_M, 0xfbbf24);
  const float H = 0.61343f * t.pitchMm;
  snprintf(b, sizeof b, "%.4f in", H / 25.4f); val(b, 10, 155, FONT_L, 0x22e664);
  snprintf(b, sizeof b, "%.3f mm", H); val(b, 10, 175, FONT_S, 0x8fa1c2);
  for (int i = 0; i < n; i++) {
    snprintf(b, sizeof b, "%.4f", H * sqrtf((i + 1) / (float)n) / 25.4f);
    val(b[0] == '0' ? b + 1 : b, 114 + (i % 5) * 40, 150 + (i / 5) * 13, FONT_S, 0xfbbf24);
  }
  if (TA.sent && TA.sent == jobId) { snprintf(b, sizeof b, "Sent as job %u. In UCCNC run M20200.", (unsigned)TA.sent); val(b, 160, 196, FONT_S, 0x22e664, middle_center); }
  else val("Tap LENGTH to type it. SEND makes the program.", 160, 196, FONT_S, 0x8fa1c2, middle_center);
}
// ---- thread presets: twelve metric and twelve inch sizes
Btn pickSysBtn(int i) { return {4 + i * 104, 35, i < 2 ? 100 : 104, 28}; }      // METRIC, INCH, BACK
Btn pickBtn(int i) { return {4 + (i % 4) * 78, 68 + (i / 4) * 56, 76, 52}; }
void drawPick(const DispSnap &s) {
  bool onArt = artBg(ART_PICK);
  if (!onArt) {
    g->fillScreen(theme().bg); drawTitle("Thread presets");
    const char *l[3] = {"Metric", "Inch", "Back"}; for (int i = 0; i < 3; i++) drawBtn(pickSysBtn(i), l[i], nullptr, theme().btn, 0xe6edf7, true);
    for (int i = 0; i < 12; i++) plainPanel(pickBtn(i).x, pickBtn(i).y, 76, 52);
  }
  ring(pickSysBtn(pickSys));
  for (int i = 0; i < 12; i++) {
    Btn b = pickBtn(i); const ThreadPreset &t = TP[pickSys][i]; char p[16];
    val(t.size, b.x + 38, b.y + 18, FONT_L, 0xe6edf7, middle_center);
    snprintf(p, sizeof p, pickSys ? "%s TPI" : "%s mm", t.pitch); val(p, b.x + 38, b.y + 38, FONT_S, 0xfbbf24, middle_center);
    if (pickSys == TA.sys && i == TA.idx) ring(b);
  }
}

// ---- Spindle Diagnostics
const Btn DG_VERDICT = {4, 207, 230, 29}, DG_BACK = {240, 207, 76, 29};
// How steady the speed has been over the last 10 s: 100 % minus the spread (standard deviation) as a share of the
// average. -1 while there is too little to go on.
float histStability() {
  const int n = min((int)histCount, 40), head = histHead;
  if (n < 8) return -1;
  float sum = 0, sq = 0;
  for (int i = 0; i < n; i++) { float v = histRpm[(head - 1 - i + 2 * HIST_N) % HIST_N]; sum += v; sq += v * v; }
  float mean = sum / n; if (mean < 1) return -1;
  float var = sq / n - mean * mean;
  return constrain(100.0f - sqrtf(max(0.0f, var)) / mean * 100.0f, 0.0f, 100.0f);
}
void drawDiag(const DispSnap &s) {
  const uint32_t GREEN = 0x22e664, AMBER = 0xfbbf24, RED = 0xf87171, GREY = 0x8fa1c2;
  bool onArt = artBg(ART_DIAG);
  if (!onArt) {
    g->fillScreen(theme().bg); drawTitle("Spindle Diagnostics");
    plainPanel(4, 35, 312, 58); plainPanel(4, 97, 312, 50); smallLabel("RPM", 14, 78); smallLabel("DEVIATION", 261, 47, middle_center);
    const char *l[6] = {"STABILITY", "SENSOR", "NOISE", "MISSED", "ACCEL", "LOAD"};
    for (int i = 0; i < 6; i++) { int x = 4 + (i % 3) * 105, y = 151 + (i / 3) * 28; plainPanel(x, y, 102, 25); smallLabel(l[i], x + 6, y + 13); }
    drawBtn(DG_BACK, "Back", nullptr, theme().btn, 0xe6edf7, true);
  }
  char b[24]; snprintf(b, sizeof b, "%d", s.target); titleRight("TARGET", b);
  const bool run = s.target > 0;
  const float dev = run ? (s.rpm - s.target) / s.target * 100.0f : 0, adev = fabsf(dev), stab = run ? histStability() : -1;
  const uint32_t dcol = !run ? GREY : adev <= 2 ? GREEN : adev <= 5 ? AMBER : RED;
  snprintf(b, sizeof b, "%d", (int)(s.rpm + 0.5f)); val(b, 128, 64, FONT_XL, run && !s.enc ? RED : dcol == GREY ? GREEN : dcol, middle_center);
  if (run) snprintf(b, sizeof b, "%+.1f%%", dev); else snprintf(b, sizeof b, "--");
  val(b, 261, 70, FONT_L, dcol, middle_center);
  float lo, hi; plotSpindle(34, 102, 276, 40, s, lo, hi);
  if (run) { snprintf(b, sizeof b, "%d", (int)hi); val(b, 9, 107, FONT_S, GREY); snprintf(b, sizeof b, "%d", (int)lo); val(b, 9, 138, FONT_S, GREY); }
  // the six readings
  const bool idx = s.zRevs > 0; const uint32_t bad = s.zMissed + s.zExtra;
  const int sensor = run && !s.enc ? 3 : !idx ? (s.enc && s.rpm > 1 ? 4 : 5) : bad == 0 ? 0 : bad * 1000 <= s.zRevs * (uint32_t)zPpr ? 1 : 2;
  const char *sName[6] = {"HEALTHY", "FAIR", "CHECK", "LOST", "A/B ONLY", "--"}; const uint32_t sCol[6] = {GREEN, AMBER, RED, RED, AMBER, GREY};
  char v[6][12]; uint32_t c[6] = {AMBER, sCol[sensor], GREY, AMBER, AMBER, AMBER};
  if (stab >= 0) snprintf(v[0], 12, "%.1f%%", stab); else snprintf(v[0], 12, "--");
  if (stab >= 0) c[0] = stab >= 98 ? GREEN : stab >= 95 ? AMBER : RED;
  snprintf(v[1], 12, "%s", sName[sensor]);
  if (!idx) snprintf(v[2], 12, "--"); else if (s.zExtra == 0) { snprintf(v[2], 12, "LOW"); c[2] = GREEN; } else if (s.zExtra * 200 <= s.zRevs) { snprintf(v[2], 12, "MED"); c[2] = AMBER; } else { snprintf(v[2], 12, "HIGH"); c[2] = RED; }
  if (idx) { snprintf(v[3], 12, "%u", (unsigned)s.zMissed); c[3] = s.zMissed ? RED : GREEN; } else snprintf(v[3], 12, "--");
  if (s.accelBusy) snprintf(v[4], 12, "..."); else if (s.accelMs < 0) snprintf(v[4], 12, "--"); else snprintf(v[4], 12, "%.1f s", s.accelMs / 1000.0f);
  snprintf(v[5], 12, "%d%%", s.load);
  for (int i = 0; i < 6; i++) val(v[i], 4 + (i % 3) * 105 + 96, 151 + (i / 3) * 28 + 13, FONT_S, c[i], middle_right);
  // verdict
  const char *vt; uint32_t fill, edge;
  if (!run) { vt = "SPINDLE STOPPED"; fill = 0x16223a; edge = 0x5b7bb0; }
  else if (!s.enc) { vt = "NO ENCODER SIGNAL"; fill = 0x5c1414; edge = RED; }
  else if (s.accelBusy) { vt = "REACHING SPEED"; fill = 0x4a3505; edge = AMBER; }
  else if (adev <= 2 && (stab < 0 || stab >= 98) && sensor != 2) { vt = "WITHIN TOLERANCE"; fill = 0x0c4a24; edge = 0x28eb6e; }
  else if (adev <= 5 && sensor != 2) { vt = "CHECK SPEED HOLD"; fill = 0x4a3505; edge = AMBER; }
  else if (sensor == 2) { vt = "CHECK THE ENCODER"; fill = 0x5c1414; edge = RED; }
  else { vt = "OUT OF TOLERANCE"; fill = 0x5c1414; edge = RED; }
  g->fillRoundRect(DG_VERDICT.x, DG_VERDICT.y, DG_VERDICT.w, DG_VERDICT.h, 7, fill); g->drawRoundRect(DG_VERDICT.x, DG_VERDICT.y, DG_VERDICT.w, DG_VERDICT.h, 7, edge);
  val(vt, 119, 221, FONT_L, edge, middle_center);
}

// ---- Material & Speed
// Surface speed in feet per minute, [material][tool: 0 HSS, 1 carbide][cut: 0 rough, 1 finish]. HSS figures are the
// two ends of the ranges in LittleMachineShop's turning table (1018-type steel, 300-series stainless, brass,
// wrought aluminium); the carbide finishing figure is that table's carbide column and roughing is 70 % of it.
// They are starting points for a small lathe, not limits: drop the speed if it chatters or the chips turn blue.
const char *MAT_NAME[4] = {"Aluminum", "Steel", "Stainless", "Brass"};
const int MAT_SFM[4][2][2] = {{{500, 600}, {2000, 2820}}, {{125, 215}, {600, 850}}, {{115, 135}, {400, 570}}, {{300, 350}, {820, 1170}}};
Btn matBtn(int i) { return {4 + i * 79, 35, 75, 30}; }
Btn matField(int i) { return {4 + i * 106, 69, 100, 44}; }
const Btn MT_APPLY = {4, 206, 100, 30}, MT_DRO = {108, 206, 104, 30}, MT_BACK = {216, 206, 100, 30};
// RPM = surface speed x 12 / (pi x diameter), rounded to 10 and kept inside Min RPM..Max RPM. asked is the
// speed before that limit.
int matRpm(int minR, int maxR, int &sfm, int &asked) {
  sfm = MAT_SFM[MS.mat][MS.tool][MS.cut];
  asked = (int)(sfm * 3.8197f / max(0.05f, MS.dia) + 0.5f);
  return constrain((asked + 5) / 10 * 10, minR, maxR);
}
float droDia() { return fabsf(xPos) * (S.xDia ? 1.0f : 2.0f); }
void drawMat(const DispSnap &s) {
  if (!artBg(ART_MAT)) {
    g->fillScreen(theme().bg); drawTitle("Material & Speed");
    for (int i = 0; i < 4; i++) drawBtn(matBtn(i), MAT_NAME[i], nullptr, theme().btn, 0xe6edf7, true);
    const char *l[3] = {"WORK DIA", "TOOL", "CUT"};
    for (int i = 0; i < 3; i++) { Btn f = matField(i); plainPanel(f.x, f.y, f.w, f.h); smallLabel(l[i], f.x + 50, f.y + 9, middle_center); }
    plainPanel(4, 117, 312, 84); smallLabel("SUGGESTED RPM", 14, 137);
    drawBtn(MT_APPLY, "Apply", nullptr, 0x166534, 0xe6edf7, true); drawBtn(MT_DRO, "Use X DRO", nullptr, theme().btn, 0xe6edf7, true); drawBtn(MT_BACK, "Back", nullptr, theme().btn, 0xe6edf7, true);
  }
  char b[64];
  if (s.preset > 0) { snprintf(b, sizeof b, "%d", s.preset); titleRight("PRESET", b); } else titleRight("SPEED", "KNOB");
  ring(matBtn(MS.mat));
  snprintf(b, sizeof b, "%.3f in", MS.dia); val(b, 54, 97, FONT_L, 0x22e664, middle_center);
  val(MS.tool ? "CARBIDE" : "HSS", 160, 97, FONT_L, 0xfbbf24, middle_center); val(MS.cut ? "FINISH" : "ROUGH", 266, 97, FONT_L, 0xfbbf24, middle_center);
  int sfm, asked; const int r = matRpm(s.minRpm, s.maxRpm, sfm, asked);
  const bool high = asked > s.maxRpm, low = asked < s.minRpm;
  snprintf(b, sizeof b, "%d", r); val(b, 196, 146, FONT_XL, high || low ? 0xfbbf24 : 0x22e664, middle_center);
  if (high || low) snprintf(b, sizeof b, "%d SFM asks %d rpm: limited by %s RPM", sfm, asked, high ? "Max" : "Min");
  else snprintf(b, sizeof b, MS.cut ? "%d SFM. Feed .002-.004/rev, cut .005-.010 in" : "%d SFM. Feed .004-.008/rev, cut .020-.040 in", sfm);
  val(b, 160, 185, FONT_S, high || low ? 0xfbbf24 : 0xe6edf7, middle_center);
}

// ---- number pad with a decimal point, for the thread length and the work diameter (inches)
const char *NP_LBL[16] = {"7", "8", "9", "Clear", "4", "5", "6", "", "1", "2", "3", "Cancel", "0", ".", "Del", "OK"};
void drawNumpad(const DispSnap &s) {
  const char *dro = npFor ? "X DRO" : "Z DRO";
  bool onArt = artBg(ART_NUMPAD);
  if (!onArt) {
    g->fillScreen(theme().bg); plainPanel(6, 4, 308, 56);
    for (int i = 0; i < 16; i++) drawBtn(kpBtn(i), i == 7 ? dro : NP_LBL[i], nullptr, i == 15 ? 0x166534 : theme().btn, 0xe6edf7, strlen(NP_LBL[i]) != 1 && i != 15);
  } else { Btn k = kpBtn(7); val(dro, k.x + k.w / 2, k.y + k.h / 2, FONT_S, 0xfbbf24, middle_center); }
  val(npFor ? "WORK DIAMETER" : "THREAD LENGTH", 18, 21, FONT_L, 0xe6edf7);
  val("inches", 18, 44, FONT_S, 0xfbbf24);
  g->setClipRect(200, 6, 110, 52); drawNumber(np[0] ? np : "0", 306, 32, 104, 52, middle_right, np[0] ? 0x22e664 : 0x3b5578); g->clearClipRect();
}

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
  dispBuffered = buffered;
  if (buffered) { art.setColorDepth(16); art.setPsram(true); artOk = art.createSprite(320, 240) != nullptr; }
  Serial.printf("display: artwork %s\n", artOk ? "buffer allocated" : "off (no memory), plain screens");
  Serial.printf("display: %s\n", buffered ? "frame buffer allocated" : "no memory for a frame buffer, drawing directly");
  lcd.fillScreen(TFT_BLACK);
  enum { M_MAIN, M_MENU, M_VIEW, M_COLORS, M_INFO, M_REBOOT, M_KEYPAD, M_TOOLS, M_THREAD, M_PICK, M_DIAG, M_MAT, M_NUM } mode = M_MAIN;
  char kp[6] = "";                                       // digits typed on the speed keypad
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
      else if (mode == M_MAIN) {
        if (S.dispView == 1 && py >= 0 && py < RPM_STRIP_H + 14) { kp[0] = 0; mode = M_KEYPAD; }   // the title strip of the RPM view
        else mode = M_MENU;
      }
      else if (mode == M_MENU) {
        if (hit(MENU_CLOSE, px, py)) mode = M_MAIN;
        else if (hit(MENU_TOOLS, px, py)) mode = M_TOOLS;
        else if (hit(MB[B_RPM], px, py)) { kp[0] = 0; mode = M_KEYPAD; }
        else if (hit(MB[B_VIEW], px, py)) mode = M_VIEW;
        else if (hit(MB[B_SWAP], px, py)) mode = M_COLORS;
        else if (hit(MB[B_ZERO_X], px, py)) zeroReq[0] = true;
        else if (hit(MB[B_ZERO_Z], px, py)) zeroReq[1] = true;
        else if (hit(MB[B_XDIA], px, py)) { S.xDia = !S.xDia; dispSaveReq = true; }
        else if (hit(MB[B_FLIP], px, py)) { S.dispFlip = !S.dispFlip; dispSaveReq = true; }
        else if (hit(MB[B_CAL], px, py)) calibrateTouch(flip);
        else if (hit(MB[B_INFO], px, py)) mode = M_INFO;
      } else if (mode == M_KEYPAD) {
        for (int i = 0; i < 16; i++) if (hit(kpBtn(i), px, py)) {
          const char *k = KP_LBL[i]; size_t n = strlen(kp);
          if (i == 3) kp[0] = 0;                                                    // Clear
          else if (i == 7) { screenRpm = 0; mode = M_MAIN; }                         // Knob: back to the knob's own speed
          else if (i == 11) mode = M_MAIN;                                           // Cancel
          else if (i == 14) { if (n) kp[n - 1] = 0; }                                // Del
          else if (i == 15) {                                                        // OK: 0 or empty also means "by knob"
            int v = atoi(kp);
            screenRpm = v > 0 ? constrain(v, S.minRpm, S.maxRpm) : 0;
            mode = M_MAIN;
          } else if (n + strlen(k) <= 5 && !(n == 0 && k[0] == '0')) strcat(kp, k);  // a digit; no leading zeros
          break;
        }
      } else if (mode == M_TOOLS) {
        if (hit(TB[0], px, py)) mode = M_THREAD; else if (hit(TB[1], px, py)) mode = M_DIAG; else if (hit(TB[2], px, py)) mode = M_MAT; else if (hit(TB[3], px, py)) mode = M_MENU;
      } else if (mode == M_THREAD) {
        if (hit(TH_BTN[0], px, py) || hit(TH_NAME, px, py)) { pickSys = TA.sys; mode = M_PICK; }
        else if (hit(TH_BTN[1], px, py) || hit(TH_PASS, px, py)) { TA.passSel = (TA.passSel + 1) % 4; TA.sent = 0; }
        else if (hit(TH_LEN, px, py)) { np[0] = 0; npFor = 0; mode = M_NUM; }
        else if (hit(TH_BTN[2], px, py)) threadSend();
        else if (hit(TH_BTN[3], px, py)) mode = M_TOOLS;
      } else if (mode == M_PICK) {
        if (hit(pickSysBtn(0), px, py)) pickSys = 0; else if (hit(pickSysBtn(1), px, py)) pickSys = 1; else if (hit(pickSysBtn(2), px, py)) mode = M_THREAD;
        else for (int i = 0; i < 12; i++) if (hit(pickBtn(i), px, py)) { TA.sys = pickSys; TA.idx = i; TA.sent = 0; mode = M_THREAD; }
      } else if (mode == M_DIAG) {
        if (hit(DG_BACK, px, py)) mode = M_TOOLS; else if (hit(DG_VERDICT, px, py)) { diagReset(); accelMs = -1; }   // a tap on the verdict starts the counts again
      } else if (mode == M_MAT) {
        for (int i = 0; i < 4; i++) if (hit(matBtn(i), px, py)) MS.mat = i;
        if (hit(matField(0), px, py)) { np[0] = 0; npFor = 1; mode = M_NUM; }
        else if (hit(matField(1), px, py)) MS.tool = !MS.tool;
        else if (hit(matField(2), px, py)) MS.cut = !MS.cut;
        else if (hit(MT_DRO, px, py)) { float d = droDia(); if (d >= 0.05f && d <= 8.0f) MS.dia = d; }
        else if (hit(MT_APPLY, px, py)) { int sfm, asked; screenRpm = matRpm(S.minRpm, S.maxRpm, sfm, asked); mode = M_MAIN; }   // the knob still starts it
        else if (hit(MT_BACK, px, py)) mode = M_TOOLS;
      } else if (mode == M_NUM) {
        for (int i = 0; i < 16; i++) if (hit(kpBtn(i), px, py)) {
          size_t n = strlen(np); float v = -1;
          if (i == 3) np[0] = 0;                                                    // Clear
          else if (i == 7) v = npFor ? droDia() : fabsf(zPos);                       // take it from the DRO
          else if (i == 11) v = 0;                                                   // Cancel
          else if (i == 14) { if (n) np[n - 1] = 0; }                                // Del
          else if (i == 15) v = atof(np);                                            // OK
          else if (n < 6 && (i != 13 || !strchr(np, '.'))) strcat(np, NP_LBL[i]);    // a digit or the one decimal point
          if (v >= 0) {                                                              // leave; a value outside 0.05..10 in is ignored
            if (v >= 0.05f && v <= (npFor ? 8.0f : 10.0f)) { if (npFor) MS.dia = v; else { TA.len = v; TA.sent = 0; } }
            mode = npFor ? M_MAT : M_THREAD;
          }
          break;
        }
      } else if (mode == M_COLORS) {
        if (hit(CB[0], px, py)) { S.dispSwapRb = !S.dispSwapRb; lcd.setColorSwap(S.dispSwapRb); dispSaveReq = true; }
        else if (hit(CB[1], px, py)) { S.dispBlack = !S.dispBlack; dispSaveReq = true; }
        else if (hit(CB[2], px, py)) mode = M_MENU;
      } else if (mode == M_VIEW) {
        for (int i = 0; i < 5; i++) if (hit(VB[i], px, py)) { S.dispView = i; dispSaveReq = true; mode = M_MAIN; }   // show it at once
        if (hit(VB[5], px, py)) mode = M_MENU;
      } else if (mode == M_INFO) {
        if (hit(INFO_REBOOT, px, py)) mode = M_REBOOT; else if (hit(INFO_BACK, px, py)) mode = M_MENU;
      } else if (mode == M_REBOOT) {
        if (hit(YES_BTN, px, py)) {
          lcd.fillScreen(theme().bg); lcd.setTextDatum(middle_center); lcd.setFont(FONT_L); lcd.setTextColor(0xe6edf7);
          lcd.drawString("Rebooting...", 160, 120); delay(300); ESP.restart();
        }
        if (hit(NO_BTN, px, py)) mode = M_INFO;
      }
    }
    if (mode != M_MAIN && mode <= M_KEYPAD && now - lastTap > MENU_TIMEOUT_MS) mode = M_MAIN;   // a menu left open goes back to the DRO; the tool screens stay
    if (tap || now - lastDraw >= DISP_FRAME_MS) {
      lastDraw = now;
      DispSnap s; xSemaphoreTake(dispLock, portMAX_DELAY); s = dispShared; xSemaphoreGive(dispLock);
      if (mode == M_MAIN) drawDro(s); else if (mode == M_MENU) drawMenu(s); else if (mode == M_VIEW) drawViewMenu(s); else if (mode == M_COLORS) drawColorsMenu(s); else if (mode == M_INFO) drawInfo(s); else if (mode == M_KEYPAD) drawKeypad(s, kp); else if (mode == M_TOOLS) drawTools(s); else if (mode == M_THREAD) drawThread(s); else if (mode == M_PICK) drawPick(s);
      else if (mode == M_DIAG) drawDiag(s); else if (mode == M_MAT) drawMat(s); else if (mode == M_NUM) drawNumpad(s); else drawConfirmReboot(s);
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
    int preset = screenRpm;
    targetRpm = (preset > 0 && t > 0) ? constrain(preset, S.minRpm, S.maxRpm) : (t / 10) * 10;
  } else targetRpm = 0;

  // Spindle Diagnostics: how long the spindle takes to come within 3 % of a new speed.
  static int accelTarget = 0; static uint32_t accelStart = 0;
  if (abs(targetRpm - accelTarget) > max(20, accelTarget / 20)) { accelTarget = targetRpm; accelStart = millis(); accelBusy = targetRpm > 0; }
  if (accelBusy && encOk && fabsf(rpm - targetRpm) <= targetRpm * 0.03f) { accelMs = (int)(millis() - accelStart); accelBusy = false; }

  // PI
  if (targetRpm == 0) { duty = 0; integ = 0; }
  else if (encOk) {
    float err = targetRpm - rpm; integ += err * dt; integ = constrain(integ, 0, 1.0f / S.ki);
    duty = S.kp * err + S.ki * integ;
  } else duty += 0.02f;                               // spindle not turning yet: ramp until pulses arrive
  duty = constrain(duty, 0.0f, 1.0f);
  pwmWrite((uint32_t)(duty * PWM_MAX));
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
  d["knob"] = S.knobEnabled; d["webOverride"] = webTarget >= 0; d["preset"] = (int)screenRpm; d["job"] = (uint32_t)jobId;
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
#if __has_include("web_page.h")
  // Arduino IDE build: the page is built into the firmware, gzipped (tools/make_arduino.py writes web_page.h from
  // data/index.html), so uploading the firmware also updates the page and no filesystem upload is needed.
  // Recordings and the board files still live in LittleFS and are served from there.
  auto page = [](AsyncWebServerRequest *r) {
    AsyncWebServerResponse *res = r->beginResponse(200, "text/html", WEB_PAGE_GZ, sizeof WEB_PAGE_GZ);
    res->addHeader("Content-Encoding", "gzip"); res->addHeader("Cache-Control", "no-cache"); r->send(res);
  };
  server.on("/", HTTP_GET, page); server.on("/index.html", HTTP_GET, page);
#endif
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
    if (!r->hasParam("pitch") || !r->hasParam("major") || !r->hasParam("len")) { r->send(400, "text/plain", "pitch, major (mm) and len (in) are needed"); return; }
    float pitch = r->getParam("pitch")->value().toFloat(), major = r->getParam("major")->value().toFloat(), len = r->getParam("len")->value().toFloat();
    int passes = r->hasParam("passes") ? r->getParam("passes")->value().toInt() : 11;
    AsyncWebServerResponse *res = r->beginResponse(200, "text/plain", threadProgram(String(major, 1) + " x " + String(pitch, 2), pitch, major, len, passes, 200));
    res->addHeader("Content-Disposition", "attachment; filename=\"thread_" + String(major, 1) + "x" + String(pitch, 2) + ".nc\""); r->send(res);
  });
  server.on("/api/job", HTTP_GET, [](AsyncWebServerRequest *r) {
    JsonDocument d; xSemaphoreTake(jobLock, portMAX_DELAY); d["id"] = (uint32_t)jobId; d["name"] = jobName; d["bytes"] = jobText.length(); xSemaphoreGive(jobLock);
    String o; serializeJson(d, o); r->send(200, "application/json", o);
  });
  server.on("/api/job.nc", HTTP_GET, [](AsyncWebServerRequest *r) {
    xSemaphoreTake(jobLock, portMAX_DELAY); String t = jobText, n = jobName; xSemaphoreGive(jobLock);
    if (!t.length()) { r->send(404, "text/plain", "(No program has been sent from the touch screen since the ESP32 started)\n"); return; }
    n.replace(" ", ""); n.replace("/", "_");
    AsyncWebServerResponse *res = r->beginResponse(200, "text/plain", t);
    res->addHeader("Content-Disposition", "attachment; filename=\"thread_" + n + ".nc\""); res->addHeader("Cache-Control", "no-store"); r->send(res);
  });
  // The UCCNC macro, written with this ESP32's own address so it works as downloaded. Save it as
  // C:\UCCNC\Profiles\Macro_<profile>\M20200.txt, then type M20200 in UCCNC's MDI line (or give it a button).
  server.on("/api/uccnc-macro", HTTP_GET, [](AsyncWebServerRequest *r) {
    String ip = (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP()).toString();
    String m = "// M20200: fetch the program sent from the lathe's touch screen (Thread Assist > SEND) and load it.\n"
               "// Made by the ESP32 lathe controller, firmware " FW_VERSION ". If the ESP32's address changes, edit the next line.\n"
               "string url = \"http://" + ip + "/api/job.nc\";\n"
               "string file = System.IO.Path.Combine(System.IO.Path.GetTempPath(), \"lathe_job.nc\");\n"
               "try {\n"
               "  using (System.Net.WebClient wc = new System.Net.WebClient()) { wc.DownloadFile(url, file); }\n"
               "  exec.Loadfile(file);\n"
               "  while (exec.IsLoading()) { System.Threading.Thread.Sleep(50); }\n"
               "  exec.AddStatusmessage(\"Lathe job loaded. Check it in the toolpath view before Cycle Start.\");\n"
               "} catch (System.Exception e) {\n"
               "  exec.AddStatusmessage(\"Lathe job not loaded: \" + e.Message);\n"
               "}\n";
    AsyncWebServerResponse *res = r->beginResponse(200, "text/plain", m);
    res->addHeader("Content-Disposition", "attachment; filename=\"M20200.txt\""); r->send(res);
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
  zPpr = max(1, (int)(S.ppr + 0.5f));
  attachInterrupt(PIN_ENC_A, isrEncA, RISING);
  attachInterrupt(PIN_ENC_Z, isrEncZ, RISING);
  axisSetup(PCNT_UNIT_0, PIN_XA, PIN_XB);
  axisSetup(PCNT_UNIT_1, PIN_ZA, PIN_ZB);
  xTaskCreatePinnedToCore(axisTask, "axes", 2048, nullptr, 5, nullptr, 1);
  attachInterrupt(PIN_CP, isrCp, RISING);
  analogReadResolution(12);
  pwmBegin(); pwmWrite(0);
  dispLock = xSemaphoreCreateMutex(); jobLock = xSemaphoreCreateMutex();
  // 24 KB stack: the library puts each smooth-font glyph bitmap on the stack while drawing it (alloca), and the
  // biggest digit is about 11 KB. With the old 6 KB stack the glyphs came out as empty boxes.
  xTaskCreatePinnedToCore(displayTask, "display", 24576, nullptr, 1, nullptr, 0);
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
  if (now - lastTx >= 250) { lastTx = now; histPush(); dispSnapshot(); broadcast(stateJson()); ws.cleanupClients(); }
  if (dispSaveReq) { dispSaveReq = false; saveSettings(); }      // a change made on the screen's menu
}
