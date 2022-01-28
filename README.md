# Setting up custom esp-idf

Probably need to download the correct toolchain:

- Download and unpack https://github.com/espressif/crosstool-NG/releases/download/esp-2021r2/xtensa-esp32-elf-gcc8_4_0-esp-2021r2-macos.tar.gz
- `cd ~/.platformio/packages/toolchain-xtensa32`
- `rm -R -- */`
- `cp -r ~/Downloads/xtensa-esp32-elf/* .`

If you want to update the version I think you need to nuke the copy in ~/.platformio/packages.
