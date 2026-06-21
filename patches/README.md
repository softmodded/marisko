# zephyr patches

`zephyr/` is pulled by `west update` and is gitignored, so these required edits to
the zephyr tree are kept here as patches. apply them after `west update`:

```
cd zephyr
git apply ../patches/zephyr/*.patch
cd ..
```

## what they do

- **0001-i2s-nrfx-slave-ratio-64x.patch** — the sp-1 runs the nRF i2s as a slave off
  the cs42l42's 64-BCLK/frame clock, but zephyr's i2s api can't set the i2s RATIO and
  defaults it to 32X → distorted audio. forces `RATIO_64X` in the slave path.

- **0002-cdc-acm-multipacket-bulk-out.patch** — the cdc-acm class enqueues one 64-byte
  bulk-OUT packet per turnaround, capping upload at ~180 KB/s. lets it enqueue large
  multi-packet transfers (nRF EasyDMA fills the whole buffer) → ~390 KB/s. needs the
  `rx-fifo-size` / `CONFIG_UDC_BUF_*` bumps already in the app overlay + prj.conf.
