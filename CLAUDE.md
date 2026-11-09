# Working rules for this project

ESP32-S3 lathe controller: firmware in `src/main.cpp` (PlatformIO, Arduino-ESP32 2.0.x), web dashboard in `data/index.html`, milled interface board drawn by `hardware/make_pcb.py`. `README.md` explains what it does; `CHANGELOG.md` is the version history; `HANDOFF.md` says where the work stands.

## Versions: every change gets one
- Firmware change: bump `FW_VERSION` in `platformio.ini` and add a `## Firmware` entry in `CHANGELOG.md`.
- Page change (`data/index.html`, anything in `data/`): bump `WEB_VERSION` in `data/index.html` and add a `## Web page` entry. Raise `FW_MIN` only when the page needs new firmware.
- Board change (anything that alters `hardware/make_pcb.py` output): bump `REV` in the script (A, B, C, ...), extend its history comment, add a row to the Interface board table in `CHANGELOG.md`, and bump the web page too, because the board files ship with it.
- A fix inside a change still gets its own bump; never fold a second fix into an entry that was already announced.

## Files the ESP32 serves
- Every file the user can download goes in `data/` (board files in `data/hw/`, written by the script), gzipped if large, and gets a link in the Help tab. Nothing lives only in the repo.
- LittleFS names must be 31 characters or fewer. The script checks this; `board_rev*` names were chosen to fit with `.gz`.
- After changing `data/`, build the image with `$env:USERPROFILE\.platformio\penv\Scripts\pio.exe run -t buildfs` and read the whole output: mklittlefs prints `error adding file!` and still ends with SUCCESS.
- Uploading the filesystem image erases saved recordings; tell the user.

## Board script
- Run from anywhere: `python -I hardware/make_pcb.py <folder containing shapely>` (shapely needs numpy). It deletes other revisions' files, writes the rev into every file and file name, sets `BOARD_REV` in `data/index.html`, and copies everything to `data/hw/`.
- The script's checks are the design rules; it exits 1 on a failure. Never hand-edit the board files.
- Mill-file layers are one operation each: ISO_CENTER (cutter centre lines for a 0.5 mm cutter, no offset), CLEAR_POCKET, COPPER_REF (reference only), DRILL_0P8 / DRILL_1P3 / ALIGN_3P0, SLOT, OUTLINE, NOTES. Keep that contract; the Help tab's milling steps describe it.
- Design constants to respect: 1.0 mm minimum gap between copper shapes (so cutter paths never merge), 8 mm surface creepage and 6 mm straight-line clearance from the lathe nets (HIGH, WIPER, LOW), alignment holes on the flip line x = 72 and not symmetric about the board centre.

## Style
- Comments and Help text are written for a hobbyist milling a board at home: plain words, say why.
- Don't commit Wi-Fi credentials or other secrets; the Settings page is where they go.
