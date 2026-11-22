/*
  ESP32-S3 lathe controller, firmware 1.15.0 with web page 1.26.0 built in.
  Arduino IDE version of the PlatformIO project (github.com/winreboot/esp32s3-lathe).

  The code is in the lathe.cpp tab; this file only carries the setup notes. lathe.cpp, fonts_vlw.h and
  web_page.h are written by tools/make_arduino.py, so make changes in src/main.cpp and run it again.

  ONE-TIME SETUP
  1. Boards Manager: "esp32" by Espressif Systems. Either the current 3.x (checked with 3.3.13) or 2.0.17 works.
  2. Library Manager: install
       - LovyanGFX by lovyan03, 1.2 or newer (checked with 1.3.0). Version 1.1.16 does not build on core 3.x.
       - ArduinoJson by Benoit Blanchon, 7.x
       - ESP Async WebServer by ESP32Async, 3.x
       - Async TCP by ESP32Async, 3.x (ESP Async WebServer needs it)
     If an older "ESPAsyncWebServer" or "AsyncTCP" (me-no-dev or mathieucarbou) is installed, remove it first.

  TOOLS MENU (must match, or the saved settings, recordings and board files are lost)
     Board:              ESP32S3 Dev Module
     USB CDC On Boot:    Enabled
     CPU Frequency:      240MHz (WiFi)
     Flash Mode:         QIO 80MHz
     Flash Size:         8MB (64Mb)
     Partition Scheme:   8M with spiffs (3MB APP/1.5MB SPIFFS)
     PSRAM:              OPI PSRAM
     USB Mode:           Hardware CDC and JTAG
     Upload Mode:        UART0 / Hardware CDC
  The partition scheme is the one PlatformIO used (default_8MB.csv), so the settings, tool list, run hours,
  recordings and the board files already on the ESP32 stay where they are.

  UPLOADING
  - USB: plug in the port marked USB or UART, pick its port, press Upload.
  - Over Wi-Fi: Sketch > Export Compiled Binary, then upload the .bin from the build folder in the page's
    Firmware window. The page is inside the firmware now, so this also updates the page.
  Either way, saved recordings are kept: nothing is written to the file system.

  WI-FI
  The Wi-Fi name and password are no longer in the code. If you have saved them in Settings before, they are
  kept. If not, the ESP32 opens the access point ESP32-Lathe (password lathe1234): join it, go to
  http://192.168.4.1, open Settings, enter your Wi-Fi and choose Save & reboot.
*/
