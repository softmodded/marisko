# contributing to marisko

this project is very early so there's no strict structure or contribution workflow yet. just open an issue or pull request and i'll figure it out.

## the BIG five rules

- when writing code you must test all 5 of these cases before merging. ignoring any of them will temporarily brick the device:

- the app must be placed at flash address `0x20000`, max size `0xdefff`
- your firmware must feed watchdog every 5 seconds or less
- lfclk and hfclk (and some peripherals like pwm2, pwm3, saadc) are already started by the bootloader. re-initializing them may fail
- the sp-1 has no hard reset so you must provide a `SYSTEM_OFF` mechanism to return to the bootloader
- `resetreas` must be cleared before entering `SYSTEM_OFF`, it's best practice to do it on boot

see the [sp-1 developer wiki](https://github.com/timknapen/SP-1-dev/wiki) for full hardware and bootloader documentation.
