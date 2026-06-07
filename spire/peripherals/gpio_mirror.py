#!/usr/bin/env python3
"""spire GPIO mirror - intercepts nRF52840 GPIO writes and maintains readable state."""

# Addresses that firmware writes to when toggling LEDs:
# NRF_P0->OUTSET = 0x50000508
# NRF_P0->OUTCLR = 0x5000050C
# NRF_P1->OUTSET = 0x50000808
# NRF_P1->OUTCLR = 0x5000080C

P0_OUTSET = 0x50000508
P0_OUTCLR = 0x5000050C
P1_OUTSET = 0x50000808
P1_OUTCLR = 0x5000080C

# Mirror addresses (where GUI will read from):
P0_MIRROR = 0x4001F000  # unused address in nRF52840 peripheral space
P1_MIRROR = 0x4001F004

# Global state shared with GUI
gpio0_state = 0
gpio1_state = 0


def gpio_write_hook(read, addr, value):
    global gpio0_state, gpio1_state
    if read:
        if addr == P0_MIRROR:
            return gpio0_state
        if addr == P1_MIRROR:
            return gpio1_state
        return None
    # Write
    if addr == P0_OUTSET:
        gpio0_state |= value
    elif addr == P0_OUTCLR:
        gpio0_state &= ~value
    elif addr == P1_OUTSET:
        gpio1_state |= value
    elif addr == P1_OUTCLR:
        gpio1_state &= ~value
    # Always allow the original write through
    return None


def apply(machine):
    sysbus = machine.sysbus
    sysbus.AddHook(P0_OUTSET, P0_OUTCLR + 0x10, gpio_write_hook)
    sysbus.AddHook(P1_OUTSET, P1_OUTCLR + 0x10, gpio_write_hook)
    sysbus.AddHook(P0_MIRROR, P1_MIRROR + 0x10, gpio_write_hook)
