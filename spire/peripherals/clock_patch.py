# spire clock patch
# Fixes Renode's nRF52840 clock model so LFCLK appears already running
# (matching real SP-1 bootloader behavior)

import pyrenode

def apply(machine):
    sysbus = machine.sysbus

    # Register offsets that Zephyr's clock driver reads
    # 0x414 = LFCLKRUN — Zephyr checks if LFCLK start was triggered
    # 0x418 = LFCLKSTAT — Zephyr checks if LFCLK is in running state
    # 0x104 = EVENTS_LFCLKSTARTED — Zephyr waits for this event

    clock_base = 0x40000000

    def lfclk_hook(read, addr, width):
        if read:
            # When reading LFCLKRUN (0x414): report as triggered
            if addr == (clock_base + 0x414):
                return 1
            # When reading LFCLKSTAT (0x418): report as running
            if addr == (clock_base + 0x408):
                return 0
            if addr == (clock_base + 0x418):
                return 1
            # When reading EVENTS_LFCLKSTARTED (0x104): report as fired
            if addr == (clock_base + 0x104):
                return 1
            # When reading LFCLKSRCCOPY (0x41C): report SYNTH
            if addr == (clock_base + 0x41C):
                return 2
        return None  # let original handler deal with it

    sysbus.AddHook(clock_base, clock_base + 0x4FF, lfclk_hook)
