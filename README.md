# Recompiling with updated RTC clock source

- `git clone https://github.com/espressif/esp32-arduino-lib-builder`
- `cd esp32-arduino-lib-builder`
- `tools/install-esp-idf.sh`
- `git --work-dir esp-idf cherry-pick d4de081`
- Uncomment 'idf.py menuconfig' in `build.sh`
- `TARGETS=esp32 ./build.sh`
- Component config > ESP32-specific > switch to 8mhz RTC clock
- Component config > Bluetooth > Bluetooth Host to Nimble
- `git cherry-pick d4de081`
- Follow the instruction to flash the bootloader (maybe not necessary?)
- `ESP32_ARDUINO=~/.platformio/packages/framework-arduinoespressif32/ tools/copy-to-arduino.sh`
- `rm -r ~/.platformio/packages/framework-arduinoespressif32/{cores,libraries,variants}`
- `cp -r components/arduino/{cores,libraries,variants} ~/.platformio/packages/framework-arduinoespressif32/`
- `cp components/arduino/tools/platformio-build.py ~/.platformio/packages/framework-arduinoespressif32/tools/`
- `rm -R -- ~/.platformio/packages/toolchain-xtensa32/*/`
- `cp -r ~/.espressif/tools/xtensa-esp32-elf/esp-2021r2-8.4.0/xtensa-esp32-elf/* ~/.platformio/packages/toolchain-xtensa32/`
