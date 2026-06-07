# building marisko

## system dependencies

**arch (pacman/yay):**
```
sudo pacman -S cmake ninja gperf ccache dfu-util dtc python python-pip wget arm-none-eabi-gcc
```

**macos:**
```
brew install cmake ninja gperf ccache dfu-util dtc python3 wget
```

## west & nRF Connect SDK

```
pip3 install --break-system-packages west
git clone https://github.com/nrfconnect/sdk-nrf --branch v3.3.0 nrf
west init -l nrf
west update
```

this pulls ~5-10 GB and may take some time, so be patient.

## zephyr sdk (cross-compiler)
`(version 0.17.0 is used because that's what i had installed at the time. maybe someone should update that?)`

```
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_linux-x86_64.tar.zst
tar xf zephyr-sdk-0.17.0_linux-x86_64.tar.zst
cd zephyr-sdk-0.17.0
./setup.sh -t arm-zephyr-eabi -h
```

add to your shell config:

```
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-0.17.0
```

## python deps

```
pip3 install --break-system-packages -r zephyr/scripts/requirements.txt
pip3 install --break-system-packages -r nrf/scripts/requirements.txt
west zephyr-export
```

## linux-only fixes

the sdk's prebuilt `dtc` may need newer glibc. symlink the system one:

```
mv zephyr-sdk-0.17.0/sysroots/x86_64-pokysdk-linux/usr/bin/dtc zephyr-sdk-0.17.0/sysroots/x86_64-pokysdk-linux/usr/bin/dtc.sdk-bak
ln -s /usr/bin/dtc zephyr-sdk-0.17.0/sysroots/x86_64-pokysdk-linux/usr/bin/dtc
```

if `gcc-12` is missing: 

```
mkdir -p ~/.local/bin && ln -s /usr/bin/gcc ~/.local/bin/gcc-12
export PATH="$HOME/.local/bin:$PATH"
```

## compile

```
west build -b sp1 -d build app -- \
  -DBOARD_ROOT=$(pwd) \
  -DZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-0.17.0
```

## convert to .bin

the sp-1 bootloader writes the app at flash offset `0x20000`. strip the padding:

```
arm-zephyr-eabi-objcopy -O binary \
  --gap-fill 0xFF \
  --remove-section=.debug_* \
  --remove-section=.comment \
  --remove-section=.ARM.attributes \
  build/app/zephyr/zephyr.elf \
  /tmp/full.bin

dd if=/tmp/full.bin of=build/sp1_firmware.bin bs=1 skip=131072
```

(`131072` = `0x20000` - the bootloader offset in bytes)

## flash

1. open https://solderless.engineering → **firmware utility**
2. upload `build/sp1_firmware.bin`
3. works with chrome or any browser with web serial

## project structure

- `app/` — firmware source (`main.c`, cmake, kconfig)
- `boards/arm/sp1/` — custom zephyr board definition for the sp-1
- `build/` — build output (ignored)
- `spire/` — renode emulator for the sp-1

## emulator

see **[spire](spire/README.md)** for the renode-based sp-1 hardware emulator. test firmware with virtual buttons, leds, audio visualization, and gdb debugging — no risk of bricking a real device.
