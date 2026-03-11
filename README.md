# DA15 - USB Audio DAC + Amplifier

The goal of this project was to create a minimal amplifier for desktop use, using only a single USB-C cable for both data and power.
The amp outputs a max of 4.2Vrms, which is often enough for a good listening experience in a small room with moderately sensitive speakers. I personally keep the volume at 30% most of the time.

- **MCU**: STM32H503
- **DAC**: PCM5102A
- **AMP**: PAM8965

![DA15 Front](Hardware/Images/Front.jpg)
![DA15 PCB Render](Hardware/Images/3D_Render.png)

For additional pictures, see [Hardware/Images](Hardware/Images/).


## Features

- **Single USB-C cable**: for both power and audio (USB Audio Class 1, 24-bit/48kHz).
- **Power** - 2 x 4.4W into 4Ω and 2 x 2.2W into 8Ω speakers (@ 0.035% THD). Can be set at max volume without losing quality.
- **USB Audio Class 1** - 24-bit/48kHz stereo with dedicated 24.576mhz audio crystal.
- **EQ** - Basic 2 bass and treble EQ or advanced EQ profiles via the [EQOS app](https://github.com/eliachiarucci/EQOS).
- **USB-C power detection** - adapts output level based on CC line voltage (500mA / 1.5A / 3A).
- **OLED UI** - SH1106 128x64 display with rotary encoder navigation.
- **DFU firmware update** - update over USB.
- **Persistent user settings** - stored in flash with wear leveling.
- **Low power consumption** - 0.5W in standby.

## Building

**Requirements:** CMake 3.22+, Ninja, ARM GCC toolchain

The [STM32 VS Code Extension](https://marketplace.visualstudio.com/items?itemName=stmicroelectronics.stm32-vscode-extension) is recommended for development.

```bash
cmake --preset Release # Debug won't work for audio playback as it's not optimised and the STM32F0 is not very powerful. 
cmake --build build/Release
```

Or clicking the "Build" button on the bottom of VSCode.

## Debugging

There are 2 debugging profiles (in the Run and Debug tab):
**STM32Cube: STM32 Launch ST-Link GDB Server**
This is the profile created by the STM32 Extension, it will use ST-Link to flash the board.

**Debug with OpenOCD**
This profile will use OpenOCD to flash the board, and will also spawn an RTT terminal with logging directly in VSCode.


## Flashing

**With ST-Link:**
```bash
openocd -f interface/stlink.cfg -f target/stm32f0x.cfg \
  -c "program build/Release/DA15.elf verify reset exit"
```

**Over USB (DFU):**
Enter DFU mode from the device menu or run `dfu-util --detach`, then:
```bash
dfu-util -a 0 -s 0x08000000:leave -D build/Release/DA15.bin
```

STM32CubeProgrammer in USB DFU can also be used.


## Project Structure

```
App/           Application firmware
├── Inc/       Headers
└── Src/       Implementation (audio pipeline, EQ, display, USB, settings)
Core/          STM32CubeMX generated HAL init code
Drivers/       STM32F0xx HAL library
Lib/           TinyUSB (USB stack), SEGGER RTT (debug)
```

## Installing ST OpenOCD for STM32H5 support (macOS)

```bash
# Install dependencies (if not already installed)
brew install automake autoconf libtool pkg-config libusb texinfo

# Unlink upstream OpenOCD if installed
brew unlink open-ocd 2>/dev/null

# Clone and build
git clone https://github.com/STMicroelectronics/OpenOCD.git
cd /tmp/st-openocd
./bootstrap
./configure --enable-stlink CFLAGS="-Wno-gnu-folding-constant"
make -j$(sysctl -n hw.ncpu)

# Install
sudo mkdir -p /opt/homebrew/bin /opt/homebrew/share/openocd/scripts
sudo cp src/openocd /opt/homebrew/bin/openocd
sudo cp -r tcl/* /opt/homebrew/share/openocd/scripts/


## Dependencies

| Library | License | Purpose |
|---------|---------|---------|
| [TinyUSB](https://github.com/hathach/tinyusb) | MIT | USB Audio Class 1 stack |
| [STM32F0 HAL](https://github.com/STMicroelectronics/stm32f0xx_hal_driver) | BSD-3-Clause | Hardware abstraction |
| [SEGGER RTT](https://www.segger.com/products/debug-probes/j-link/technology/about-real-time-transfer/) | BSD-1-Clause | Debug output (optional) |

## Licenses

The software of this project is licensed under the [GPL-3.0-only](LICENSE).

All the hardware files are licensed under the [CERN-OHL-S-2.0](Hardware/LICENSE).

