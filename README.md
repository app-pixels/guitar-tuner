# guitar-tuner

**Guitar Tuner** · v1.0.0

Chromatic guitar tuner using the on-board mic.

**Hardware:** Waveshare ESP32-S3 1.8" AMOLED Touch

**Tags:** `#tool` `#audio` `#offline`

Pick a string; the tuner shows the detected note, the cents you're off, and a moving needle that turns green when you land on pitch. FFT pitch detection runs on the codec input at 16 kHz.

## Controls
- **BOOT** — cycle string presets (E A D G B e or chromatic)
- **PWR** — toggle mic gain

## `setup.txt` keys (all optional)
- `TUNER_MODE` — `AUTO` or `MANUAL` (default `AUTO`)
- `TUNER_MIC_GAIN` — `0`–`7` (default `5`)
- `TUNER_CENTS_TOL` — in-tune tolerance in cents (default `3`)

## Editing `setup.txt`
The device reads `/setup/setup.txt` from the SD card on boot. [Download a working sample](https://sosbxffigpteqilpgxwn.supabase.co/storage/v1/object/public/app-assets/setup/setup.txt) — covers every app — and edit the keys you need.

Don't want to eject the card? Use the [**USB Stick**](/apps/usb-stick) app (mounts the SD card as a USB drive over USB-C) or the [**Filehub**](/apps/filehub) app (edit over WiFi).

## Build

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x.
2. Add the ESP32 board package (≥ 3.1.0):

   ```
   arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```

3. Install the required Arduino libraries:

   - Adafruit XCA9554
   - GFX Library for Arduino (moononournation)
   - XPowersLib (lewishe)

4. Compile and upload:

   ```
   FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,UploadSpeed=921600,LoopCore=1,EventsCore=1'
   arduino-cli compile -b "$FQBN" --build-path /tmp/guitar-tuner_build .
   arduino-cli upload  -b "$FQBN" --input-dir /tmp/guitar-tuner_build -p /dev/ttyACM0 .
   ```

   For browser flashing without a build environment, use the [pre-built binary](https://www.app-pixels.com/apps/guitar-tuner).

## License

MIT — see [LICENSE](LICENSE). Do whatever you want with it.

---

Part of the [app-pixels.com](https://www.app-pixels.com) catalogue · live listing: https://www.app-pixels.com/apps/guitar-tuner
