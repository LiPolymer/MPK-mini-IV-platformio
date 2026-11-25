# MPK mini IV Application Template (PlatformIO)

English | [简体中文](README_CN.md)

A bare-metal C++17 / C11 PlatformIO template and BSP (`mpk4-bsp`) for writing custom firmware on the **Akai Professional MPK mini IV** (`AT32F405RCT7` @ `216 MHz`, `256 KiB` flash, `96 KiB` SRAM, USB `09E8:005D`). Adapted and patched from the reverse-engineering work in [open-mpk4](https://github.com/giusmarci/open-mpk4).

The starter app in [src/main.cpp](src/main.cpp) centers `hello world!` on the `160x80` screen and sets up the main loop for panel inputs and USB MIDI.

> [!CAUTION]
> **Disclaimer & Hardware Risk:** Flashing custom bare-metal firmware carries inherent risks. Although the stock bootloader and fault-recovery paths reduce the chance of bricking, bugs in your code, misconfigured peripherals/clocks, power loss during flashing, or flash wear can still cause data loss, malfunction, or permanent hardware damage, and may void your manufacturer warranty.
>
> This project is provided **"as is"** without any warranty of any kind. Back up your presets before flashing, and proceed entirely at your own risk.

## Prerequisites

- [PlatformIO](https://platformio.org/)
- Python 3 (on first run, `tools/mpk4_flash.py` installs `hidapi` via `pip` if missing, plus `mido` and `python-rtmidi` on Linux/macOS)

## Build and Flash

### 1. Build the app package

```sh
pio run -e mpk4
```

This builds with `custom_firmware_version = 41.00` from [platformio.ini](platformio.ini) and outputs `.pio/build/mpk4/mpk4_app_package.bin`. The package only contains your application binary, version metadata, storage bounds, and a SHA-256 footer, with no Akai code included. A prebuilt sample is also in [release/mpk4_app_package.bin](release/mpk4_app_package.bin).

### 2. Fetch the stock firmware (first time only)

Because this repo does not redistribute Akai binaries, the flasher builds the full `256 KiB` flash image locally on top of Akai's `v1.41` or `v1.43` stock firmware (verified by SHA-256 in [tools/mpk4_layout.py](tools/mpk4_layout.py)). Fetch and verify it once:

```sh
# Downloads only the ~80 KB firmware payload from Akai's public installer
python tools/fetch_stock.py

# Or extract it offline from a local .pkg, Payload, cpio, or stock .bin
python tools/fetch_stock.py --from-file path/to/stock_or_installer
```

The verified image is cached at `~/.openmpk4/MPKminiIV_v1.41_stock.bin` (override with `$OPENMPK4_HOME`; `backup/firmware/MPKminiIV_v1.41_stock.bin` is checked first if present).

### 3. Flash to hardware

**Note:** Flashing erases `0x06000–0x3EFFF`, which includes user presets and slot storage. Back up any presets you want to keep beforehand. Factory serials, wheel calibration, and `0x3F000–0x3FFFF` are preserved.

On Windows, close any DAW or MIDI app holding the MPK mini IV port before flashing, as `mpk4_flash.py` sends the update-mode SysEx directly over Windows Kernel Streaming (`usbaudio.sys`).

```sh
pio run -e mpk4 -t upload
# or invoke the flasher directly:
python tools/mpk4_flash.py flash .pio/build/mpk4/mpk4_app_package.bin --write
```

### Dry-run, standalone image, and recovery

You can inspect the generated image and flash plan offline without touching USB:

```sh
python tools/mpk4_flash.py flash .pio/build/mpk4/mpk4_app_package.bin
python tools/mpk4_flash.py flash .pio/build/mpk4/mpk4_app_package.bin --stock path/to/stock.bin
python tools/mkimage.py .pio/build/mpk4/mpk4_app_package.bin full_image.bin
```

Other useful flasher commands:

```sh
python tools/mpk4_flash.py probe               # check mode ('app' or 'bootloader <ver>')
python tools/mpk4_flash.py enter-update --send # send SysEx F0 47 00 5D 74 00 00 F7
python tools/mpk4_flash.py flash stock --write # restore Akai stock firmware
```

If your firmware hangs and stops responding to SysEx, unplug USB, hold **SHIFT + UNDO**, and plug it back in until `Upgrade` shows on the display. Hard faults in [startup.c](lib/mpk4-bsp/src/hal/startup.c) also reboot straight into bootloader update mode so the board stays recoverable.

## Writing Your Firmware

Your code lives in [src/main.cpp](src/main.cpp). Set `custom_firmware_version` in [platformio.ini](platformio.ini) in `NN.NN` BCD format (checked by [scripts/build.py](scripts/build.py)).

A few BSP quirks to keep in mind:

- **Init & watchdog:** Call `mpk4::Board::begin()` and `mpk4::Board::enableWatchdog()` at startup, then `mpk4::Board::poll()` at the top of every loop iteration. If `Board::begin()` returns `false`, the `12 MHz` crystal failed to start and the MCU falls back to `8 MHz` HICK; USB will be unavailable, though panel controls and DIN MIDI still work.
- **Keep polling USB MIDI:** Even if you don't handle incoming MIDI, keep calling `mpk4::UsbMidi::read(packet)`. It watches cable 0 for `F0 47 00 5D 74 00 00 F7` and reboots into the bootloader on request.
- **Non-blocking display DMA:** `mpk4::Display::draw()` returns `false` while SPI2 DMA is still sending the previous frame (`~15.2 ms`); just retry on a later loop pass. `drawGreeting()` in [src/main.cpp](src/main.cpp) only includes glyphs for `"hello world!"` and can be deleted once you add your own UI.
- **Flash storage stalls:** If you use persistent slots ([storage.h](lib/mpk4-bsp/include/mpk4/storage.h)), call `storage_init()` at boot and `storage_poll(idle)` in the loop. Sector erases stall flash reads for `~16 ms` (the `50 kHz` timer tick and SysTick stay running in SRAM), so only pass `idle = true` when a brief stall is fine, such as `Controls::idleMillis() >= 200`.

### BSP Modules ([bsp.hpp](lib/mpk4-bsp/include/mpk4/bsp.hpp))

| Module | Summary |
| :--- | :--- |
| `mpk4::Board` | Clock/peripheral init, `poll()`, watchdog, `millis()`, `ticks()` (`20 µs`), `serial()`, `rebootToBootloader()` |
| `mpk4::Controls` | Input queue (`read()`), live key/pad/button/ADC/knob/wheel/encoder/pedal getters, key curves `0..2` |
| `mpk4::Leds` | 32-level PWM: `pad(index, r, g, b)`, `button(btn, brightness, secondColour)`, `beatButton(...)` |
| `mpk4::Display` | `160x80` RGB565 `Canvas` (`clear`, `pixel`, `fillRect`), `rgb565()`, non-blocking DMA `draw()` |
| `mpk4::UsbMidi` | 5 host-to-device / 4 device-to-host USB-MIDI 1.0 cables (MIDI, DAW, Plugin, Software Control, DIN Out) |
| `mpk4::DinMidi` | `31250` baud DIN MIDI out on `PB6` (`write()`, prioritized `realtime()`, `queued()`) |
| `mpk4::MidiClock` | `20–300` BPM `24 PPQN` clock on the `50 kHz` timer tick, with optional DIN forwarding and beat LED sync |
| [storage.h](lib/mpk4-bsp/include/mpk4/storage.h) | C API for 19 power-fail-safe flash slots (`STORAGE_MAX_BLOB = 12144` bytes) |

## Input Events (`mpk4::InputEvent`)

Defined in [bsp.hpp](lib/mpk4-bsp/include/mpk4/bsp.hpp). Each event returned by `mpk4::Controls::read(event)` has `{ kind, index, value, delta, sample }`:

| `InputKind` | `index` | `value` | `delta` | `sample` | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `keyOn` | Key `0..24` (`0` = lowest C) | Velocity `1..127` | `1` if odd contact closed first, else `0` | Contact time `dt` in `20 µs` ticks (max `16383`) | Uses active `keyCurve` |
| `keyOff` | Key `0..24` | `0` | `0` | `0` | — |
| `buttonDown` | `uint8_t(Button)` | `0` | `0` | `0` | `10 ms` press debounce |
| `buttonUp` | `uint8_t(Button)` | `0` | `0` | `0` | Emitted immediately on release |
| `knob` | Knob `0..7` (`K1..K8`) | `0` | Signed ticks (`2°` per tick) | `ms` per tick (capped at `200`) | After `200 ms` idle, needs `6°` to wake (`±1`) |
| `encoder` | `0` | `0` | Signed detents | `0` | Push switch arrives as `Button::encoder` |
| `padOn` | Pad `0..7` | Velocity `1..127` | `0` | 10-bit peak ADC (`pad_raw >> 2`) | Peak sampled over `~20 ms` (`>= 209`) |
| `padPressure` | Pad `0..7` | Pressure `1..127` | `0` | Current 10-bit ADC sample | Starts `200 ms` after `padOn`, updates every `~50 ms` |
| `padOff` | Pad `0..7` | `0` | `0` | `0` | Drops below `62` after `~20 ms` hold window |
| `wheel` | `0` (`pitch`) / `1` (`modulation`) | `0` | Pitch `-8192..8191`, Mod `0..127` | Hysteresis-filtered 12-bit ADC | Pitch has `±300` deadzone around centre |
| `pedal` | `0` | Pin level (`1` = high/open) | `0` | `0` | `5 ms` debounce on `PA7` |
| `matrix` | Drive line `0..10` | Closed sense bits (`1` = closed) | `0` | Changed sense bits | Only when `enableRawEvents(true)` |
| `adc` | ADC ch `0..5` | Mux pos `0..7` | `0` | 12-bit ADC value | Only when `enableRawEvents(true)` (`>= 32` delta) |

`mpk4::Button` indices: `0` `octaveDown`, `1` `octaveUp`, `2` `arp`, `3` `latch`, `4` `noteRepeat`, `5` `tap`, `8` `bank` (bicolour), `9` `minus`, `10` `plus`, `11` `shift`, `12` `daw` (bicolour), `13` `undo`, `14` `encoder` (no LED), `16` `loop`, `17` `play`, `18` `record`, `19` `overdub`.

## Flash Layout and Repo Structure

Flash layout defined in [ld/app.ld](ld/app.ld) and [tools/mpk4_layout.py](tools/mpk4_layout.py):

| Address | Size | Region |
| :--- | :--- | :--- |
| `0x08000000–0x08005FFF` | `24 KiB` | Akai bootloader (copied from stock) |
| `0x08006000–0x0800BFFF` | `24 KiB` | Staged bootloader copy (must match bootloader) |
| `0x0800C000–0x0802BFFF` | `128 KiB` | Application (`FLASH`, vector table at `0x0800C000`) |
| `0x0802C000–0x080387FF` | `50 KiB` | Slot storage (`STORAGE`, 25 x `2 KiB` sectors, erased on update) |
| `0x08038800–0x08038FFF` | `2 KiB` | App-valid marker sector (`0x38FFE–0x38FFF` from stock) |
| `0x08039000–0x0803EFFF` | `24 KiB` | Unused stock app-data pages (filled with `0xFF`) |
| `0x0803F000–0x0803F7FF` | `2 KiB` | Protected backup page (untouched by updates) |
| `0x0803F800–0x0803FFFF` | `2 KiB` | Settings page & trailer (serial, calibration, version, checksum) |

Key paths:
- [src/main.cpp](src/main.cpp): application entry point
- [platformio.ini](platformio.ini) & [boards/mpk_mini_iv.json](boards/mpk_mini_iv.json): PlatformIO environment and board definition
- [lib/mpk4-bsp/](lib/mpk4-bsp/library.json): BSP (`TinyUSB 0.21.0`, ArteryTek `AT32F402/405` library `v2.1.5`)
- [scripts/](scripts/package.py): pre/post build and packaging hooks (`build.py`, `package.py`)
- [tools/](tools/mpk4_flash.py): stock fetcher and flasher scripts (`fetch_stock.py`, `mkimage.py`, `mpk4_flash.py`, `mpk4_layout.py`)

## License

Project code is MIT-licensed ([LICENSE](LICENSE)). Third-party component licenses and trademark notices are in [NOTICE](NOTICE). This project is independent and not affiliated with Akai Professional or inMusic Brands, Inc.
