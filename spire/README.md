# spire

**sp-1 emulator for renode** 

## what it does

- emulates the sp-1's nrf52840, leds, buttons
- shows a virtual device window with clickable buttons and live led feedback
- can be used for easier debugging custom firmware

## install

### renode

```bash
# i use arch so this is a guide for arch-based 
yay -S renode-bin # or your favorite aur helper
```

### python deps

```bash
pip3 install tkinter
```

## usage

### quick start

```bash
cd spire
./run.sh
```

this opens renode with the sp-1 platform and the virtual device gui.

### load firmware

in the renode console:

```
include @sp1.resc firmware=../build/app/zephyr/zephyr.elf
```

## virtual device

| component | emulated? | notes |
|-----------|-----------|-------|
| nrf52840 cpu | full | cortex-m4, 1mb flash, 256kb ram |
| gpio / leds | visual | 4 playback + 4 track leds |
| function button | clickable | p0.27, active low with pull-up |
| play/track buttons | clickable | resistor ladder via adc |
| i2c bus | stub | cs42l42 at 0x48, tas2505 at 0x18 |
| i2s audio | waveform + wav | 48khz 24-bit output captured |
| emmc storage | stub | 4gb block device |
| wdt | full | nrf52840 hardware model |
| power / system_off | full | power peripheral model |

## project structure

```
spire/
├── sp1.repl              # renode platform description
├── sp1.resc              # renode script (load + start)
├── run.sh                # launcher (renode + gui)
├── peripherals/
│   └── sp1_gui.py        # virtual device gui
└── README.md
```

## license

MIT
