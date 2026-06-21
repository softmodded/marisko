# marisko

**custom firmware for the teenage engineering sp-1 stem player.**  

named after the [marisko flower](https://en.wikipedia.org/wiki/Cypripedium_calceolus)

## features

`this project is in the very early stages of development, so as of now pretty much anything could pass as a "feature"`

- bouncing light animation across the 4 playback leds
- function button powers off device and returns it to the bootloader
- watchdog is fed preventing bootloops

## return to bootloader

the sp-1 has no hard reset so to get back:

1. press **function** and wait for the leds go dark, this means device shut off
2. press any button (or plug in usb) — bootloader opens
3. use the [solderless firmware utility](https://solderless.engineering) to flash new firmware

## rome

**[rome](https://github.com/softmodded/rome)** is the companion cli — flash firmware and upload/manage stems on the device over USB.

## building

see **[building.md](BUILDING.md)** for setup, compilation, and flashing instructions.

## contributing

see **[contributing.md](CONTRIBUTING.md)** for rules & guidelines.

## credits

- **[zephyr rtos](https://www.zephyrproject.org/)** + **[nrf connect sdk](https://www.nordicsemi.com/Products/Development-software/nrf-connect-sdk)** — the foundation this firmware runs on
- **[sp-1 developer wiki](https://github.com/timknapen/SP-1-dev)** by tim knapen — hardware documentation, pinouts, and bootloader specs
- **[solderless](https://solderless.engineering)** — the web-based firmware and stem loader that makes all of this possible without opening the device

## license

MIT
