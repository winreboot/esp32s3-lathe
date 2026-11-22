# Version history

The firmware and the web page have separate version numbers, and both appear in the page header and the Firmware window.

| Part | Set in | How to install |
|---|---|---|
| Firmware | `FW_VERSION` in `platformio.ini` | Firmware window (OTA), or PlatformIO Upload |
| Web page | `WEB_VERSION` in `data/index.html` | PlatformIO Upload Filesystem Image, which erases saved recordings |

`FW_MIN` in `data/index.html` is the oldest firmware the page works fully with. Raise it when a page change depends on new firmware.

Change either part, then bump its version and add an entry here.

## Firmware

### 1.15.0
- **Programs from the web page for UCCNC.** `POST /api/job?name=...&file=...` with the G-code as the body (64 KB at most) makes it the job, the same slot SEND on the touch screen's Thread Assist uses, so the `M20200` macro in UCCNC loads it. The newest program replaces the last; it is kept until the ESP32 restarts. `GET /api/job` now also gives `file` and `source` (`screen` or `web`), and `/api/job.nc` downloads under the job's own file name (letters, digits, `.` `-` `_`, at most 40 characters). Each upload is logged in Events.
- Nothing else changes: the touch screens are as in 1.14.0.

### 1.14.0
- **Tools: three new touch screens**, opened with the new TOOLS button in the Settings title bar. Each has artwork for both backgrounds and a plain version, like the other screens, and stays open until Back is pressed.
- **Thread Assist.** Twelve metric and twelve inch presets on their own screen, the length typed on a number pad (or taken from the Z DRO), 6, 8, 11 or 14 passes plus two spring passes, and the thread depth and the depth after every pass shown before anything is cut. SEND makes the G33 program and keeps it on the ESP32 as a job: `/api/job` describes it, `/api/job.nc` is the program. The ESP32 cannot write onto the PC, so the PC fetches it: the page's Thread Wizard shows a Download link, and `/api/uccnc-macro` hands out a UCCNC macro (`M20200.txt`, written with the ESP32's own address) that downloads the job and loads it with `exec.Loadfile`. The macro has not been run on a real UCCNC yet.
- `threadProgram()` rewritten to match the page's Thread Wizard with its default choices: any number of passes with equal metal per pass, the fit check about three quarters of the way, X moved before Z on the way in, a header that states the passes. `/api/thread.nc` takes an optional `passes` and answers 400 instead of crashing when a value is missing.
- **Spindle Diagnostics.** Speed, deviation from the target in percent, the speed and drive graph, stability over the last 10 s, the time taken to reach the last new speed, load, and a verdict bar. New: the encoder's index pulse (Z, GPIO 6) is now read. The A pulses between two index pulses are compared with Encoder lines per turn: fewer are counted as missed pulses, more as noise. Without an index signal those readings show A/B ONLY and dashes. A tap on the verdict bar restarts the counts.
- **Material & Speed.** Aluminum, steel, stainless or brass; HSS or carbide; rough or finish; work diameter typed or taken from the X DRO. Suggests RPM = surface speed × 3.82 ÷ diameter, limited to Min and Max RPM, with a feed and depth-of-cut hint. APPLY sets it as the screen preset. Surface speeds are from LittleMachineShop's turning table (carbide roughing is 70 % of their carbide figure).
- Two more text fonts, `fonts/t_mid.vlw` (values) and `fonts/t_xl.vlw` (thread names and the large numbers: capitals, digits and a few signs). Twelve more pictures in `art/`. The live state has a new `job` number.
- Built for cores 2.0.17 and 3.3.13 (about 2.3 MB and 2.5 MB of the 3.2 MB app space). Not yet run on the display or against a real encoder index: the layouts were checked on PC previews of the same artwork.

### 1.13.0
- **The black background has the glow style too.** A second set of artwork, `art/*_k.png`: a black ground with the same glowing blue edges, shaded panels, icons and title bars. Colours > Background now switches between the two sets at once. The plain flat screens remain only as the fallback without PSRAM or if a picture fails to decode. About 150 KB more firmware.

### 1.12.0
- **Screens with glow, shading and icons.** Every screen now sits on artwork: a dark blue background that is lighter in the middle, panels and buttons with a gradient, a bright edge and a soft outer glow, line icons on the Settings buttons, and the title bars. `tools/make_art.py` draws the parts that never change into `art/*.png` (eight pictures, about 190 KB), which are linked into the firmware; the firmware decodes the picture for the screen in use into its own PSRAM buffer and paints the live values on top.
- RPM view on the artwork: title bar with a spindle icon, direction pill and PRESET/SET speed; the speed in a new 80 px digit font (`fonts/d_rpm.vlw`) with an RPM label under it; the graph in its own panel with the scale at the left; a bottom bar with the tool, LOAD % and a menu button. Settings, the keypad, Info, View, the everything view and the X and Z view have artwork too; Colours, the single-axis views and the reboot question use the glowing background with code-drawn buttons in matching colours.
- The plain screens of 1.11.0 remain and are used with the **black** background, without PSRAM, or if a picture fails to decode. Button positions are the same in both, so touch behaves the same.
- Opening a screen takes about a fifth of a second longer the first time, while its picture is decoded. Built for cores 2.0.17 and 3.3.13 (about 1.7 MB of the 3.2 MB app space); the look was checked against PC previews of the same artwork, not yet on the display.

### 1.11.0
- **Set the speed on the screen.** A keypad (Settings > Set RPM, or a tap on the title strip of the RPM only view) takes a speed. The knob stays the start and stop: at zero the spindle is off; turned up, the spindle runs at the typed speed instead of the knob position, and the screen shows PRESET. The Knob key, or OK with nothing typed, gives the speed back to the knob. The preset is limited to Min and Max RPM, is not saved across a restart, and a target from the web page still overrides it. With "Knob controls spindle" set to No, the preset does nothing.
- **RPM only view: a graph instead of the speed bar and the status line.** The last 37 s of measured speed (green, in a window of ±20 % round the target, which is the dotted line) and controller drive (amber, 0 to 100 %), so a dip under load and the recovery can be seen. Below it: the loaded insert or the alarm, and LOAD %. The "CNC offline" and IP line is gone from this view; CNC state is now on the Info screen.
- **Text in Oswald, like the digits.** Titles, labels and buttons use two new anti-aliased fonts, `fonts/t_big.vlw` and `t_small.vlw` (made by `tools/make_vlw_text.py`), instead of the library's bitmap fonts, which remain the fallback.
- **Menus redrawn.** Settings is a title bar with Close and a 3 × 3 grid: Set RPM, View, Colours, Zero X, Zero Z, X shows, Screen, Touch, Info. Reboot moved onto the Info screen. Buttons have a lighter edge, and two-line buttons show their current value in amber.
- The live state sent to the page has a new `preset` value. Built for the Arduino IDE on cores 2.0.17 and 3.3.13; not yet run on the display, so spacing may need a touch-up.

### 1.10.1
- The Arduino IDE sketch now also builds with the current Arduino-ESP32 core 3.x (checked with 3.3.13), not only 2.0.17. Core 3.x replaced `ledcSetup`/`ledcAttachPin` with `ledcAttachChannel`, and its `ledcWrite` takes the pin instead of the channel; `pwmBegin()` and `pwmWrite()` pick the right calls for the installed core. Nothing changes on 2.0.x or in the PlatformIO build.
- With core 3.x the Arduino build needs LovyanGFX 1.2 or newer (checked with 1.3.0); 1.1.16 does not compile against it. The partition table is the same on both cores, so settings, recordings and board files survive a change of core.
- On core 3.x the serial log shows a warning at boot that the legacy pulse-counter driver is deprecated. It still works; nothing else in the firmware uses the new driver, so the two do not conflict.

### 1.10.0
- **Arduino IDE version.** `tools/make_arduino.py` writes `arduino/esp32s3_lathe/`, a sketch folder that builds in the Arduino IDE (esp32 core 2.0.17, the core PlatformIO uses). `src/main.cpp` stays the only copy of the code; the script copies it in and adds the firmware version, the digit fonts as C arrays (`fonts_vlw.h`, because the IDE has no `embed_files`) and the web page, gzipped (`web_page.h`). The setup notes and the exact Tools menu settings are at the top of `esp32s3_lathe.ino`.
- In the Arduino build the **page is built into the firmware**: `/` and `/index.html` come from flash, so uploading the firmware, over USB or in the Firmware window, also updates the page, with no filesystem upload and no lost recordings. Recordings and the board files still come from LittleFS. The PlatformIO build is unchanged and still serves the page from LittleFS.
- The Wi-Fi name and password are no longer in the source code. Saved settings are kept; with none saved, the ESP32 opens the ESP32-Lathe access point so they can be entered in Settings. The old values are still in the repository's history, so change that Wi-Fi password.

### 1.9.0
- Display: a **Colours** screen (the Settings button that was Swap red / blue) with two switches: **Swap red / blue**, with the R G B check squares, and **Background**: dark blue or **black**. Black puts everything on a black screen; the row panels become black too and keep a thin grey outline so the layout still reads. Both are saved.

### 1.8.3
- Display: the smooth digits no longer sit in dark boxes. They are now drawn without a background colour, which makes the library read the frame buffer behind each glyph and blend the edges onto it, so the digits are transparent over the row colour.

### 1.8.2
- Display: the smooth digits now actually appear. The font files listed the glyphs as 0–9 then point and minus, but the library finds a glyph with a binary search over the table, which needs it sorted by character code; with minus (45) last, every digit was reported missing and drawn as a placeholder box. The fonts are regenerated in code-point order (`-` `.` `0`–`9`). The 24 KB display stack from 1.8.1 stays, because the library still copies each glyph bitmap onto the stack.

### 1.8.1
- Display: fixes the smooth digits showing as empty boxes in 1.8.0. The graphics library copies each glyph bitmap onto the stack while drawing it (up to 11 KB for the biggest digit), which overflowed the display task's 6 KB stack. The task now has 24 KB.

### 1.8.0
- Display: smooth digits. The numbers were the library's 48 px seven-segment bitmap font scaled up 1.5×, which made every pixel a block. They are now anti-aliased glyphs (0–9, point, minus) from **Oswald Bold** (SIL Open Font License, `fonts/OFL.txt`) in three sizes: 120 px digits for the RPM-only view, 70 px for the DRO rows and the single-axis views, 57 px when a value has two digits before the point. Each number uses the largest size that fits its space, so nothing is ever scaled.
- The fonts are `fonts/d_big.vlw`, `d_mid.vlw` and `d_small.vlw` (about 140 KB), linked into the firmware by `board_build.embed_files` and read straight from flash; `tools/make_vlw.py` regenerates them from the TTF. If they ever fail to load, the old bitmap font is used and the serial log says so.

### 1.7.0
- Display: a **View** button in the Settings menu chooses what fills the screen: Everything (the three rows), **RPM only**, **X only**, **Z only**, or **X and Z**. The choice is saved. Pick RPM only until the axis encoders are fitted, so the screen does not show two axes at zero.
- RPM only: the speed in digits up to 120 px tall, with direction, the target, a full-width speed bar with the target marked, the loaded tool or the alarm, encoder state, and a status line with CNC and Wi-Fi. X only and Z only: the axis in the same large digits, with the other axis and the spindle small underneath. X and Z: two tall rows.
- The Settings menu's bottom row is now View, Info, Close, Reboot.
- No firmware change was needed for the level shifter modules that replace the milled interface board: the spindle A/B direction sense does not depend on signal polarity (both lines invert together), the index line is not used, and the charge pump is timed, not read.

### 1.6.1
- Display: the R, G and B colour-check squares in the Settings menu move to the lower right corner of the Swap red / blue button, where they no longer sit on top of its label.

### 1.6.0
- Display: the boot screen is now a drawn lathe (bed, headstock, chuck, brass workpiece, tool post, tailstock) with the name and firmware version, shown for 2 s. It replaces the plain blue test screen.
- Display: the Settings menu has no header any more; a new **Info** button shows the firmware version, Wi-Fi address and signal, the `.local` address, the spindle run hours and the powered-on hours. Any tap on the Info screen goes back.
- Display: the bottom menu row is Info, Close, Reboot; the other buttons are a little taller.
- The spindle run time itself was already counted (every second the spindle turns above 10 rpm) and saved every minute; this release only puts it on the screen.

### 1.5.0
- Display: the touch panel is now used. Wire T_CLK to GPIO 12 and T_DIN to GPIO 11 (in parallel with the display's SCK and SDI), T_DO to GPIO 13 and T_CS to GPIO 14; T_IRQ stays unconnected. GPIO 14 is no longer offered as a blue LED pin.
- Display: a tap anywhere on the DRO opens a Settings menu: Zero X, Zero Z, X shows diameter/radius, screen normal/upside down, swap red/blue, calibrate touch, close, and Reboot (asks for confirmation). It closes by itself after 30 s. Changes are saved to flash.
- Display: the first tap runs a three-point touch calibration (press three crosses); it can be repeated from the menu. The calibration is stored.
- Display: a red/blue swap setting for panels whose colour order is reversed; the menu shows R, G and B squares to check. The DIA, LONG and FWD sub-labels are now near white, so they read well with either colour order (they blended into the background before).
- Display: a small three-line mark in the X row's corner shows where the menu is.

### 1.4.2
- Display: the SPI clock drops from 40 to 20 MHz. At 40 MHz over jumper wires the panel can miss its start-up commands and stay asleep, which looks like a dead screen. A frame now takes about 60 ms, well inside the 200 ms refresh.
- Display: a self-test at boot, drawn straight to the panel: a blue screen with the firmware version and "display OK" for 1.5 s. If that shows, the wiring and panel are good.
- Display: if the frame buffer cannot be allocated (no PSRAM, or memory already used), the numbers are drawn directly to the panel instead of nothing. The serial log says which path is in use.

### 1.4.1
- Display: removes the Wi-Fi, LED and CNC status bar. X, Z and spindle each get an 80-pixel row, and their numbers are scaled up to 1.5× (about 13 mm tall on a 2.8 in screen) so they can be read from a distance.
- The spindle row shows RPM large on the left, with the target, speed bar and loaded insert (or an active alarm) in a column on the right.

### 1.4.0
- Replaces the TM1637 4-digit display with a 2.8 in ILI9341 color display (LovyanGFX): status bar, X and Z DRO, RPM with speed bar and target, loaded tool, and alarms.
- Display on SPI2: SCK 12, MOSI 11, CS 10, DC 9, RESET 8. GPIO 21 and 47 are free.
- Adds a Display upside down setting. Blue LED pin choices no longer include GPIO 8–13.

### 1.3.0
- The X and Z DRO now reads a 600 P/R E6B2-CWZ6C encoder on each slide, instead of counting G540 step pulses.
- The encoders are counted by the ESP32-S3's hardware pulse counters in x4 quadrature (2400 counts per revolution), with a 6.25 µs noise filter.
- Replaces the X and Z steps/unit settings with encoder P/R, travel per encoder revolution and an X diameter setting.
- Pins: X A/B on GPIO 7/15 and Z A/B on GPIO 16/17 (previously X and Z step and direction).

### 1.2.0
- RGB LED: green breathing when idle, blue flashing while reading the encoder or step lines, recording or playing back.
- Blue LED: solid on Wi-Fi, slow flash while looking for it, very fast flash when it cannot be found.
- Adds LED pin and polarity settings and the `ledTest` WebSocket command.

### 1.1.0
- Stores the tool list and the loaded tool in NVS, so they survive firmware and filesystem updates.
- Adds `GET` and `POST /api/tools` and the `toolLoad` WebSocket command.
- Reports the loaded tool in the live state.

### 1.0.0
- Initial release: spindle control, DRO, recorder, OTA and web dashboard.

## Interface board

`REV` in `hardware/make_pcb.py`. Bump it for every change to the board; the script writes it into every file name (`board_revD_bottom.dxf` ...), into each file, and into `BOARD_REV` in the page. The board files ship with the web page, so each revision also has a web page entry below.

| Rev | Web page | Board |
|---|---|---|
| F | 1.18.0 | Mill files redrawn as one operation per layer (ISO_CENTER cutter centre lines, CLEAR_POCKET, COPPER_REF, drills by size, SLOT, OUTLINE rectangle); 6 mm straight-line gap through the slot; pads grown where there is room; asymmetric alignment holes; 2 mm slot |
| E | 1.17.0 | 1.0 mm minimum gap between copper shapes (was 0.5), so the cutter path round each shape is a separate closed loop; smaller pads; bottom ground pieces joined through GND vias |
| D | 1.16.0 | Pull-ups R6, R8, R10, R12 moved to 3.81 and 7.62 mm from the transistors; bottom ground at least 1 mm wide, so no slivers or bubbles between pads |
| C | 1.15.0 | Two-sided: top traces and via pins replace wire links W1–W5, top ground with one isolation ring per lead, alignment holes, CLEAR layer |
| B | 1.14.0 | 144 × 94 mm single-sided, for a 150 × 100 blank |
| A | 1.13.0 | 165 × 118 mm single-sided, first milled version |

## Web page

### 1.26.0 (needs firmware 1.4.0; Send to UCCNC needs 1.15.0)
- **New G-code page** with six generators. Each has its parameters, a to-scale toolpath preview (cutting moves and rapids), warnings, the program text, **Download** and **Send to UCCNC**:
  - **Parting off.** Zig-zag ramp (the blade crosses a groove a little wider than itself, going deeper each time, so it never takes a full-width plunge) or step plunges (pecks at two or more places across the groove with a back-off before each one). The extra width goes toward the chuck so the part keeps its length; the program stops at a web diameter you choose (or cuts through), slows the feed below a diameter, can leave the groove every few zigs to clear chips, and works with the blade touched off on either corner. The depth step is the deepest bite anywhere, and the last crossing flattens the floor.
  - **Grooving.** Grooves wider than the tool: side-by-side plunges, then a finishing pass on the right wall, the floor and the left wall. For thread relief, O-ring and circlip grooves.
  - **Facing.** Passes from the outside to the centre (or to a bore), a lighter finishing pass, and a little past the centre so no pip is left.
  - **Turning.** Roughing to a diameter over a length, a finishing pass that squares the shoulder, and an optional 45° chamfer at the end (roughed in steps when it is bigger than one depth of cut). Tailstock mode parks the tool out in X instead of along Z.
  - **Taper.** Roughing passes that follow the taper, a finishing pass along it with a lead-in on its own line, the shoulder squared; shows the included angle, the half angle and the taper per foot.
  - **Peck drilling.** For a drill held on centre in the tool post: pecks out of the hole or a 0.010 in back-off, depth to the full diameter or to the point.
- Every program: inch, X as diameter, Z0 on the end face, G0 and G1 only, feeds per minute from feed per revolution × RPM, the tool out in X before it travels along Z, plain ASCII comments. Warnings for Max RPM and the soft limits from Settings. The values are remembered in the browser.
- Checked with a cutting simulation of 33 parameter sets: no rapid passes through metal, no cut is deeper than asked, and the finished shapes match within 0.0003 in.
- **Thread Wizard: Send to UCCNC** next to Download.
- The line about the program waiting for UCCNC shows above both the Thread Wizard and the generators, and says whether it came from the touch screen or this page.

### 1.25.0
- Thread Wizard: when a program has been sent from the touch screen's Thread Assist (firmware 1.14.0), a line at the top names it and links to the G-code and to the UCCNC macro.
- Help > display: new notes for the Tools screens (Thread Assist and how the program reaches UCCNC, Spindle Diagnostics and what each reading means, Material & Speed and where its figures come from).

### 1.24.1 (needs firmware 1.4.0; describes firmware 1.13.0)
- Help, Display: both backgrounds now have the artwork screens.

### 1.24.0 (needs firmware 1.4.0; describes firmware 1.12.0)
- Help, Display: explains the artwork screens and when the plain ones are used.

### 1.23.0 (needs firmware 1.4.0; describes firmware 1.11.0)
- Help, Display: describes the redrawn Settings menu, setting the speed on the screen with the keypad, and the graph in the RPM only view.

### 1.22.0 (needs firmware 1.4.0)
- Thread Wizard: **thread length is in inches** for every thread type, with the mm value shown beside it.
- Thread Wizard: **Passes** box next to the first pass. Type the number of passes and the first pass follows, or type the first pass and the number of passes follows. Two spring passes are always added. A typed first pass no longer forces at least 11 passes, so a warning appears when the first cut is heavy for a small lathe.
- Thread Wizard: **Start clearance**: how far in front of the end face each pass starts. The default is two pitches (at least 0.08 in), down from 0.2 in. A warning appears when flank infeed would leave less than one pitch of run-up.
- Thread Wizard: **Tailstock center in the end**. When ticked, the program never moves further out in Z than the start; for the fit check and at the end it pulls the tool out in X (1 in over the retract diameter) instead of going to Z0.4, so it cannot hit the live center.
- Thread Wizard: **Start at pass** writes a program that begins at a later pass, for finishing a thread after a stop; the file name gets `_from_passN`. **Pass schedule** lists each pass with its depth, X and start Z.
- Thread program: approaches the work with X first, then Z, instead of one diagonal rapid. Choosing a different pitch now resets the speed, passes and start clearance for that pitch.
- The firmware's `/api/thread.nc` still writes the older fixed program (11 passes, start Z0.2); the wizard does not use it.

### 1.21.1 (needs firmware 1.4.0)
- Help, Display: describes the Colours screen (red/blue swap and the black background) of firmware 1.9.0.

### 1.21.0 (needs firmware 1.4.0)
- Help: ready-made **level shifter modules** replace the milled interface board as the standard build. New **Level shifters** tab with every pin of LS1 (spindle lines, 12 V on the HV side), LS2 (X and Z encoders, 12 V) and LS3 (charge pump, 5 V), and the PC817 module that stays for spindle control. The wiring diagram, its connection table, Start here, Parts, G540, Build steps, Setup and checks and Troubleshooting follow. The milled board keeps its tab as "Milled board (alt.)".
- Help, Display: describes the View choice (Everything, RPM only, X only, Z only, X and Z).

### 1.20.1 (needs firmware 1.4.0)
- Help, Build steps: the lathe pot voltage limit is 25 V (C1's rating), not the PC817's 35 V. Matches the Display and Interface board text.
- New `docs/BUILD_GUIDE.md` in the repository: the build guide with pictures (dashboard, Help panes, board drawings, the display screens), linked from the README.

### 1.20.0 (needs firmware 1.4.0)
- Help, Display: describes the lathe boot picture, the Info button (firmware, Wi-Fi, address, spindle and power-on hours) and the header-less menu of firmware 1.6.0.

### 1.19.0 (needs firmware 1.4.0)
- Help, Display: the wiring table now includes the touch pins (T_CLK → 12, T_DIN → 11, T_DO → 13, T_CS → 14) and explains the module's J1 jumper: VCC goes to 3V3 when J1 is bridged (the DIANN board), to 5V when it is open. The display section describes the on-screen Settings menu and the first-tap calibration. Build steps and troubleshooting updated to match.
- Help: the screen preview shows the white sub-labels and the menu mark.

### 1.18.0 (needs firmware 1.4.0)
- Interface board **rev F**, after a desktop-CNC review of the rev E files. The old mill files put the copper outlines and their holes on one COPPER layer and near-duplicate outlines on CLEAR, so a CAM program had to be told which lines to offset which way. Now every layer is one operation:
  - **ISO_CENTER**: the cutter's centre lines for a 0.5 mm cutter, to follow with no offset. There is exactly one closed line round every copper shape and hole (the script checks this on both sides), and the ISO groove plus the pockets provably remove every piece of copper that does not belong to a shape.
  - **CLEAR_POCKET**: areas to pocket, now running 1 mm past the board edge. **COPPER_REF**: the copper that stays, for checking only.
  - **DRILL_0P8**, **DRILL_1P3**, **ALIGN_3P0**: holes split by drill size. **SLOT**: the isolation slot as its own closed shape (now 2.0 mm wide, for a 1.5 mm or smaller end mill). **OUTLINE**: a plain rectangle, so a wide outline cutter never meets the slot.
  - The DXF header states millimetres (`$INSUNITS`), and a NOTES line in each file explains the layers.
- Lathe-side spacing: besides the 8 mm surface path round the slot, no copper on either side is closer than 6 mm to the lathe nets in a straight line through the slot (was 4.9 mm). The script reports both figures.
- Small pads grow from 1.5 mm to 2.2 mm across wherever their neighbours leave the 1.0 mm gap and 1.0 mm of ground between them (20 of 50 pads), for a wider copper ring round the hole; the rest stay at 1.5 mm.
- The two alignment holes are no longer symmetric about the board centre (now at y = 3.5 and 74.2 mm on the centre line), so the flipped board only fits the pins the right way round.
- The CAM SVGs draw the ISO groove at its true 0.5 mm width, so they show what actually comes off. Help's milling steps and layer list follow the new layers.

### 1.17.0 (needs firmware 1.4.0)
- Interface board **rev E**, for clean CAM toolpaths. Where copper shapes were 0.5 mm apart, the cutter paths on each side overlapped, and the CAM drew merged bubble and bone-shaped loops.
- The minimum gap is now 1.0 mm everywhere, on both sides, and between traces, pads and ground. With a cutter up to 0.5 mm wide, every copper shape gets one separate closed path.
- To make room:
  - Small pads are 1.5 mm across (were 1.8), and via pads 2.0 mm (were 2.4).
  - The spindle Z trace moved 0.8 mm right.
  - Bottom ground pieces that the traces now cut apart are joined through GND vias and the top ground. There are 15 via pins (V8–V15 are GND).
- New design check: it grows every copper outline by a 0.5 mm cutter's radius and fails if any two paths would merge. All checks pass.
- Help: milling cutter size, via count, parts table and build steps.

### 1.16.0 (needs firmware 1.4.0)
- Interface board **rev D**. Every board file name now carries the revision, for example `board_revD_bottom.dxf`. The names are short because the ESP32's file system can't store names over 31 characters; the script now checks this. Help shows the revision, and its download links follow it.
- Bottom side cleaned up. R6, R8, R10 and R12 stand 3.81 and 7.62 mm right of the transistors instead of 2.54 and 5.08. The ground fill must now be at least 1 mm wide, so it no longer squeezes into the gaps between pads as small bubbles and half-moon slivers. Where it can't fit, the gap is on the CLEAR layer and gets pocketed.
- The top side follows the moved holes. All design checks pass on both sides.

### 1.15.0 (needs firmware 1.4.0)
- Interface board is now **two-sided**, for a double-sided blank, with no plated holes. The layout, size (144 × 94 mm) and every terminal are unchanged.
- The top copper replaces wire links W1–W5 with two traces: +12V from J3 to J1 and J2, and 3V3 joining the four 3V3 strips. They connect to the bottom through via pins V1–V7, bare wires soldered on both sides.
- The rest of the top is ground copper, tied to bottom ground by GND via pins V8–V14. Every hole that is not GND sits in its own copper-free circle (0.6 mm beyond the hole edge), so the isolation cut leaves a single groove around each lead and no lead can short to it. The circles never touch, so 0.5 mm of ground copper stays between neighbouring holes even at 2.54 mm pitch, with nothing to pocket.
- Neither side has copper within 8 mm of the lathe side.
- Two 3 mm alignment holes on the vertical centre line locate the board when it is flipped for side 2.
- New CLEAR layer: copper to remove completely, so no loose copper island can bridge the lathe side to the ESP32 side. The design checks now also cover the top: isolation, creepage, connectivity through vias, and hole spacing. All pass.
- Files renamed:
  - `interface_board_mill.dxf` / `.svg` are now `interface_board_bottom.dxf` / `.svg` (side 1).
  - `interface_board_top.dxf` / `.svg` are now the side 2 mill files.
  - The checking view with part outlines is now `interface_board_parts.dxf`.
  - `interface_board_files.zip` is replaced by `interface_board_gerber.zip`, which holds the Gerber and drill files only, so the board files take about 460 KB of the file system.
- Help: parts list (double-sided blank, alignment pins, bare wire for vias), board description, file table, milling steps for two sides, parts table and build steps. Build step ticks reset.

### 1.14.0 (needs firmware 1.4.0)
- Interface board resized from 165 × 118 to 144 × 94 mm, to mill from a 150 × 100 mm blank with 3 mm spare all round.
- Terminals rearranged to fit:
  - J4 (12 V) moves to the top left corner, beside J5.
  - The encoder terminals lose their separate SH screw. Each shield now shares the 0V/SH screw with the blue wire, so J1 and J2 are 4-way and J3 is 5-way (35 ways in total).
- J8's pin order is unchanged.
- The isolation slot now opens through the top edge, and the outline is a single closed cut that includes it. All design checks pass.
- Help: board size, parts list, ground table, parts table, circuit diagram and build steps updated. Adds milling depth for 1.5 mm stock, and a warning to strip the second copper side of a double-sided blank.

### 1.13.3 (needs firmware 1.4.0)
- Adds `interface_board_mill.svg` (mirrored) and `interface_board_top.svg`: the board at true size in millimetres for CAM programs that import SVG, with COPPER, DRILL and OUTLINE layers. Both are on the ESP32 under `/hw/` (stored gzipped), linked in Help, and included in `interface_board_files.zip`.
- The colored board picture is now `interface_board_preview.svg` (was `board.svg`), also linked in Help.

### 1.13.2 (needs firmware 1.4.0)
- All board files are now on the ESP32 under `/hw/`, with download links on the Help Interface board tab: `interface_board_mill.dxf`, `interface_board_top.dxf`, `interface_board_report.txt` and `interface_board_files.zip`.
- The DXFs are stored as `.dxf.gz`; the web server sends them gzip-encoded and the browser saves a plain `.dxf`.
- `interface_board_gerbers.zip` is replaced by `interface_board_files.zip`, which holds both DXFs, the report and a `gerber/` folder.
- The DXFs are about 3× smaller (pads are 32-point circles, within 0.005 mm; coordinates to 1 µm). All the board files together take about 400 KB of the 1.5 MB file system.

### 1.13.1 (needs firmware 1.4.0)
- The Gerber and drill zip is now stored on the ESP32 at `/hw/interface_board_gerbers.zip`, with a download link on the Help Interface board tab. The DXFs stay in `hardware/` (too large for the file system).

### 1.13.0 (needs firmware 1.4.0; the display preview matches 1.4.1)
- Interface board is now a single-sided milled PCB, 165 × 118 mm. `hardware/make_pcb.py` draws it, runs design checks (net connectivity, 0.5 mm isolation, one ground copper area, 8 mm creepage to the lathe side) and writes the mill DXF (mirrored), a top-view DXF, Gerber and drill files, and pictures.
- Help, Interface board tab: board picture with the encoders, supply, G540, ESP32 and lathe wired in their real colors; the CAM files; milling steps; and why the encoders need pull-ups even on short cables.
- New connector numbering: J7 is GPIO 4, 5, 6, 18; J8 is GPIO 2, GND, GPIO 7, 15, 16, 3V3, 17. Wire links W1–W5 added. Parts list, parts table, wire table and build steps updated to match (build step ticks reset).
- Help: display preview matches the 1.4.1 layout. Removes the note that the display shows the access point address.

### 1.12.0 (needs firmware 1.4.0)
- Help: new Interface board tab with the full circuit, where each part goes, ground for every cable, and an explanation of the PC817.
- Help: new G540 tab showing every terminal and DB25 pin, and which are used.
- Help: the spindle encoder now lands on the interface board, which feeds the G540. Display wiring, parts and steps updated for the 2.8 in display.
- Settings: Display upside down.

### 1.11.0 (needs firmware 1.3.0)
- Updates Help for the X and Z axis encoders: new wiring diagram, parts list, build steps, calibration method and troubleshooting.
- Settings: encoder P/R and travel per revolution for each axis, with a live DRO resolution readout.

### 1.10.0 (needs firmware 1.2.0)
- Adds a Help window with a color-coded wiring diagram, parts list, step-by-step build checklist, setup checks and troubleshooting.
- The wiring guide powers the TM1637 display from 3.3 V instead of 5 V, to protect the ESP32 pins.

### 1.9.0 (needs firmware 1.2.0)
- Adds status LED pin settings and a Test LEDs button to Settings.

### 1.8.0 (needs firmware 1.1.0)
- Enlarges the DRO readouts and aligns the X and Z numbers on their decimal points.

### 1.7.0 (needs firmware 1.1.0)
- The lathe drawing on Overview and the Recorder page now shows the loaded tool, including the threading holder and selected insert.

### 1.6.0 (needs firmware 1.1.0)
- Adds Threading to the tool type list, with a holder and insert picker and a to-scale holder drawing.
- Redraws threading inserts from the catalog outline, with each tooth using the thread form's flank angles.
- The Thread Wizard now recommends inserts that fit the selected holder, and says when a thread needs a different holder.

### 1.5.0 (needs firmware 1.1.0)
- Adds the Tools page, the loaded tool in the header, and a check of the loaded tool against the insert a thread needs.
- Adds stock diameter to the Thread Wizard. Retract clearance follows the stock, and oversized stock gets a separate OD turning program.
- Shows firmware and page versions in the header and the Firmware window.
- Adds a browser tab icon.

### 1.4.0
- Adds Acme, Stub Acme, Trapezoidal, Buttress, Sawtooth, Square, Whitworth, BSP, Round and NPT threads.
- Recommends a cutting tool, anvil and carbide grade for each thread.
- Generates thread G-code in the browser, with depth and infeed for each form.

### 1.3.0
- Reduces Overview to the main cards. Wi-Fi, Firmware and Settings become header buttons that open windows.
- Adds the lathe drawing with live carving, screen-only Preview and timeline review.
- Scales the gauge from Max RPM in Settings.
- Clears the playback spindle target when Stop is pressed.

### 1.2.0
- Adds the Thread Wizard for UNC, UNF and ISO metric threads, with a dimensioned drawing.

### 1.1.0
- Adds navigation icons, and each menu item opens its own page.

### 1.0.0
- Initial dashboard.
