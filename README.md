# ESP32-S3 CNC Lathe Control

Web dashboard served by an ESP32-S3 on the lathe: live DRO, spindle gauge and closed-loop speed control, Wi-Fi, OTA updates, recording/playback with G-code export, threading presets, surface-speed calculator, run hours, job notes, events and alarms.

Builds with Arduino-ESP32 core 2.0.x (PlatformIO `espressif32 @ 6.9.0`) and the libraries in `platformio.ini`. The full build guide, wiring diagram and parts list are under **Help** on the dashboard; the same guide with pictures is in [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md). Version history is in `CHANGELOG.md`.

## What the ESP32 does, and doesn't
| Feature | How |
| --- | --- |
| Live DRO | Reads a 600 P/R E6B2-CWZ6C encoder on each slide, counted in hardware at 2400 counts per revolution. Zero buttons set the ESP32's own offset; zero it when you zero UCCNC. |
| Spindle RPM / direction | Encoder A/B, same as the small LED display. |
| Target RPM | From the knob (pot), or from the web page (overrides the knob until you press **Knob**). PI loop drives the lathe's controller through the PC817. |
| Spindle load | Drive effort (PWM duty) the loop needs to hold the target. Not motor current. |
| CNC Online | The G540 charge-pump signal is present, i.e. UCCNC is running and out of reset. |
| Recording | 10 samples/s of X, Z, RPM, duty while UCCNC runs. Save to flash as CSV. |
| Playback | Replays the **spindle** speed profile and scrubs the chart. The axes are UCCNC's: use **Export G-code** to get a `.nc` of the recorded moves and run that in UCCNC. |
| Threading presets | Shows pitch/TPI/depth/blank, and downloads a ready G33 program (inch, same passes as `M12x1.25_thread_2in_INCH.nc`). |
| OTA | Upload `firmware.bin` from the page. Optional manifest URL for "available version". |
| Alarms | Encoder lost while commanded; tool within 0.05 in of a soft limit (limits in Settings). |
| Run hours | Spindle and power-on hours, saved to flash every minute. |

It never drives the steppers and never replaces the lathe's E-stop.

## Wiring (ESP32-S3-DevKitC-1)
The full build guide, with diagrams, is under **Help** on the dashboard. Every encoder and G540 line reaches the ESP32 through a ready-made 4-channel BSS138 level shifter module: LS1 for the spindle A/B/Z taps (shared with the G540 inputs), LS2 for the X and Z encoders, LS3 for the optional charge pump. LS1 and LS2 have the 12 V encoder supply on their HV side, so the cables carry a 0–12 V swing and the module's own 10 kΩ pull-ups do the pulling; LS3 has 5 V. The LV sides are fed from 3V3, so the ESP32 only ever sees 3.3 V. Spindle control stays isolated through a 1-channel PC817 module. The milled two-sided interface board in `hardware/` does the same job and remains an alternative.

| ESP32 pin | Signal | From |
| --- | --- | --- |
| GPIO 4 / 5 / 6 | Spindle encoder A / B / Z (100 P/R) | LS1 LV1 / LV2 / LV3; HV1–HV3 on the encoder black / white / orange wires, which also go to G540 terminals 2 / 3 / 1. LS1 HV = 12 V, LV = 3V3 |
| GPIO 7 / 15 | X encoder A / B (600 P/R) | LS2 LV1 / LV2; HV1 / HV2 on the encoder black / white. LS2 HV = 12 V, LV = 3V3 |
| GPIO 16 / 17 | Z encoder A / B (600 P/R) | LS2 LV3 / LV4; HV3 / HV4 on the encoder black / white |
| GPIO 18 | Charge pump (optional) | LS3 LV1; HV1 on G540 DB25 pin 16, DB25 pin 18 to GND. LS3 HV = 5V pin |
| — | Encoder power | All three encoders from a **12 V** supply (the HV side must match the G540's 12 V inputs). Brown to 12 V +; blue, shields and supply − to ESP32 GND and G540 T12. Never from G540 T11. Axis encoder orange (index) unused. |
| GPIO 1 | Speed knob wiper | 10k pot between 3V3 and GND |
| GPIO 2 | Spindle PWM | PC817 module IN+ (IN− to GND). Module output C → lathe pot HIGH, E → WIPER, with 4.7k and 10 µF from WIPER to LOW. The lathe side is isolated: never joined to any other ground |
| GPIO 8 / 9 / 10 / 11 / 12 | Display RESET / DC / CS / SDI (MOSI) / SCK | 2.8" ILI9341 SPI TFT with XPT2046 touch. VCC to 3V3 if the module's J1 jumper is bridged, 5V if open; LED to 3V3; SDO unconnected |
| GPIO 12 / 11 / 13 / 14 | Touch T_CLK / T_DIN / T_DO / T_CS | T_CLK and T_DIN share the display's SCK and SDI pins. T_IRQ unconnected. A tap on the screen opens the Settings menu (zero X/Z, diameter, flip, colour swap, calibrate, reboot) |
| 5V / GND | Power | USB charger. GND common with encoder 0 V and G540 T12. **Never** with the controller board. |

Status LEDs are on the board and need no wiring. The RGB LED defaults to GPIO 48 and the blue LED to GPIO 44; change them under Settings if your board differs. Some clone boards need their "RGB" solder pads bridged before the RGB LED works.

Charge pump tap (optional, only for the CNC Online indicator): put a DB25 male-female breakout between the UC100 and the G540 and pick off pin 16.

If an axis counts backwards, set **Invert X/Z count** in Settings.

## Build and flash
1. Install PlatformIO (VS Code extension or `pip install platformio`).
2. `pio run -t upload` (firmware), then `pio run -t uploadfs` (the web page in `data/`).
3. First boot: no Wi-Fi configured, so it opens access point **ESP32-Lathe** (password `lathe1234`). Join it, open http://192.168.4.1, fill in Settings → Wi-Fi SSID/password, **Save & reboot**.
4. Afterwards it's at http://esp32-lathe.local or the IP shown on the page. Later firmware goes in through the OTA card.

## Settings to enter
Spindle encoder PPR 100 · X and Z encoder P/R 600 · X and Z travel per encoder revolution = your screw leads in inches (X radial; calibrate with a dial indicator) · X shows diameter Yes · Max RPM to suit · KP 0.0004 · KI 0.0012 · soft limits to suit your travel.

## Files
- `src/main.cpp` firmware
- `data/index.html` dashboard (open it directly in a browser to see it run on simulated data)
- `data/hw/` board pictures and CAM files, served by the ESP32 and linked from Help (written by `make_pcb.py`; don't edit by hand)
- `hardware/make_pcb.py` draws the two-sided interface board (no plated holes; vias are wire pins), checks both sides and writes the CAM files next to it. Every name carries the board revision (`REV` in the script, listed in `CHANGELOG.md`), for example rev F: `board_revF_bottom.dxf` (side 1, mirrored) and `board_revF_top.dxf` (side 2), the same two as true-size `.svg` for CAM that imports SVG, `board_revF_parts.dxf` (top view with part outlines, for checking), `_preview.svg`, `_gerber.zip` (Gerber and Excellon) and `_report.txt`. Each mill-file layer is one machining operation: ISO_CENTER (cutter centre lines for a 0.5 mm cutter, no offset), CLEAR_POCKET (pocket out), COPPER_REF (reference only), DRILL_0P8 / DRILL_1P3 / ALIGN_3P0, SLOT and OUTLINE. The script also sets `BOARD_REV` in `data/index.html`, so Help links to the new files. It also copies them all into `data/hw/`, so rebuild the filesystem image afterwards. Edit the placement in the script and rerun it (`pip install shapely`, then `python make_pcb.py`) to change the board.
- `platformio.ini`
