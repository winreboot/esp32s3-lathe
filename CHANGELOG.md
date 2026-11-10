# Version history

The firmware and the web page have separate version numbers, and both appear in the page header and the Firmware window.

| Part | Set in | How to install |
|---|---|---|
| Firmware | `FW_VERSION` in `platformio.ini` | Firmware window (OTA), or PlatformIO Upload |
| Web page | `WEB_VERSION` in `data/index.html` | PlatformIO Upload Filesystem Image, which erases saved recordings |

`FW_MIN` in `data/index.html` is the oldest firmware the page works fully with. Raise it when a page change depends on new firmware.

Change either part, then bump its version and add an entry here.

## Firmware

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
