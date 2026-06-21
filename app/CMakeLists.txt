cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sp1-phase0)

target_sources(app PRIVATE
  src/main.c
  src/leds.c
  src/saadc.c
  src/pwm.c
  src/emmc.c
  src/disk.c
  src/usb.c
  src/codec.c
  src/audio.c
)
