# spire

**sp-1 emulator for renode.** named after the norwegian word for sprout — because this is where firmware grows safely before it goes to real hardware.

## what it does

- emulates the sp-1's nrf52840, leds, buttons, i2c bus, i2s audio, and adc
- shows a virtual device window with clickable buttons and live led feedback
- visualizes audio output as a waveform
- can record i2s audio to a .wav file
- connect gdb for proper debugging — breakpoints, step-through, the works

## install

### renode

```bash
# arch
yay -S renode-bin

# macOS
brew install renode

# linux (manual)
wget https://github.com/renode/renode/releases/latest/download/renode-*.linux-portable.tar.gz
tar xf renode-*.tar.gz
export PATH="$PATH:$(pwd)/renode_*/"
```

### python deps

```bash
pip3 install tkinter  # usually bundled with python
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

### with gdb debugging

terminal 1 — start renode with gdb server:

```
renode --console -e "using sysbus; machine LoadPlatformDescription @sp1.repl; machine StartGdbServer 3333"
```

terminal 2 — connect and debug:

```
arm-zephyr-eabi-gdb ../build/app/zephyr/zephyr.elf
(gdb) target remote :3333
(gdb) break main
(gdb) continue
```

### record audio output

the gui window has an audio waveform display. to record to a .wav file, add this to the renode console before starting:

```
python "import sp1_gui; sp1_gui.record_audio('output.wav')"
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
