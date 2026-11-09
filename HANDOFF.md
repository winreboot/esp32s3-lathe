# Hand-off: where the work stands (9 November 2026)

Read `CLAUDE.md` for the working rules, `README.md` for what the system does, `CHANGELOG.md` for every version. This file is the context a new session would otherwise lack: what was decided recently, why, and what is next.

## Current versions
- Firmware **1.4.1** (`platformio.ini`), web page **1.18.0** (`data/index.html`, `FW_MIN` 1.4.0), interface board **rev F** (`hardware/make_pcb.py`).
- `data/hw/` holds the rev F files and the filesystem image builds cleanly with all 10 files. Whether the user has uploaded 1.18.0 to the ESP32 is not known.

## The interface board, and how it got to rev F
The user mills the board at home on a desktop CNC router from the DXFs the script writes, two-sided, with no plated holes (via pins are bare wire soldered both sides). The sequence of revisions matters because each one answers a problem seen on the real machine:
- **C** two-sided (top traces and via pins replaced wire links; top ground with one isolation ring per lead; alignment holes).
- **D** pull-ups spread out, bottom ground at least 1 mm wide: no slivers between pads.
- **E** 1.0 mm minimum gap between all copper shapes. The user's CAM drew merged "bubble" and "bone" loops wherever shapes were 0.5 mm apart, because the cutter paths on each side of the gap overlapped. The gap, not the drawing, was the cause. Pads shrank to 1.5 mm to make room; bottom ground pieces are joined through GND vias and the top ground.
- **F** (latest) followed an outside CNC review of the rev E files (`PCB_RevE_CNC_Review_and_Recommendations.txt`, in the user's Downloads, not in the repo). Accepted and implemented: one machining operation per layer (ISO_CENTER cutter centre lines for a 0.5 mm cutter, CLEAR_POCKET, COPPER_REF, drills split by size, SLOT as its own 2.0 mm shape, OUTLINE as a plain rectangle); `$INSUNITS` in the DXF header; 6 mm straight-line clearance through the slot to the lathe nets (was 4.9); pads grown to 2.2 mm where room allows (20 of 50; the 30 at 2.54 mm pitch cannot grow without breaking the 1.0 mm gap); alignment holes made asymmetric (y = 3.5 and 74.2 on x = 72) so the board cannot be fitted upside down; CAM SVGs draw the ISO groove at true width.
- Deliberately **not** changed: the CLEAR pockets still touch the copper. Pulling them back to the ISO groove would leave a floating copper strip whenever the user's V-bit cuts narrower than 0.5 mm. The pocket's first pass re-running the ISO groove is intended.
- The script now checks: one ISO path per copper ring on both sides, and that ISO groove plus pockets remove every piece of copper that belongs to no shape.

## Electrical caution carried over from the review
The lathe speed-pot circuit (HIGH, WIPER, LOW at J9) may float at mains potential. The board's answer is the PC817 optocoupler plus the 8 mm surface path and 6 mm air gap; it is not a certified barrier. Help now tells the user to measure HIGH–LOW (must be under 25 V, C1's rating) and each pin against earth before connecting J9. Don't weaken the slot, the gaps or the keep-outs.

## Open items
1. **Not yet verified in the user's CAM program.** Next step for the user: load `board_revF_bottom.dxf`, set up ISO_CENTER as an on-line engrave (cutter ≤ 0.5 mm at depth), CLEAR_POCKET as a pocket, drills, SLOT as a through pocket (≤ 1.5 mm end mill), OUTLINE as an outside profile; confirm the preview shows separate closed loops; then mill both sides. Any merged loops are a cutter-width or offset setting, not the file.
2. **Wi-Fi credentials in `src/main.cpp`**: the `Settings` struct defaults (`ssid`, `pass`, around line 44) contain the user's real network name and password. They should be empty strings (the README says a fresh board opens its own access point). Fix this before the repository is made public; it is a firmware change, so bump `FW_VERSION` and add a CHANGELOG entry.
3. The user's PCB blanks warped when heated; the two-sided design exists partly so no copper has to be stripped. Keep both sides' files from the same revision.

## Tooling notes
- PlatformIO: `$env:USERPROFILE\.platformio\penv\Scripts\pio.exe` (not on PATH). `run -t buildfs` builds the image; `run -t uploadfs` uploads it (erases saved recordings).
- Board script: `python -I hardware/make_pcb.py <folder with shapely and numpy>`. ezdxf is handy for validating the DXFs (R12, closed POLYLINEs; the parts file has intentional open polylines for TO-92 outlines).
- The Help tab in `data/index.html` builds the board download links from `const BOARD_REV`, which the script rewrites.
