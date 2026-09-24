# Roadmap

**English** · [中文](ROADMAP_CN.md)

Roughly ordered. Items are sized so a newcomer can pick one up. **Comment on the issue (or open one) before starting**, so
work does not collide.

## Display path — push fewer bytes (highest value)

Logical speed is already full speed (59-60 fps), but only ~15 frames/sec are actually pushed to the panel.
The obvious fix — deeper queue + triple buffering — **was measured and reverted** (see README): it doubled drawn frames
(15 -> 30/sec) but periodically wedges the panel, because 1280x720@60 Hz DPI scan-out already consumes ~106 MB/s of PSRAM
bandwidth and leaves no arbitration headroom for a second concurrent writer.

So the safe direction is to move **less** data per frame, not to add concurrency:

- [ ] **Submit only the rows the core actually changed.** GBA titles frequently update a small band of the screen; the framework
      already tracks partial frames. Start by measuring how many rows a typical game changes per frame.
- [ ] Measure and publish the drawn-fps before/after in the PR description.
- [ ] Ship it behind a runtime switch (SD-card file), defaulting to the current behaviour.

Related, lower priority:

- [ ] Investigate whether the transpose step can be avoided entirely by having the core render into the panel's byte order.
- [ ] Re-evaluate PPA/DMA2D hardware scaling **with throttling** (small chunks only, never a full-screen burst). The naive version
      is known to wedge the DSI — see the notes in BUILDING.md.

## Hardware features

- [ ] **Battery gauge** — the Tab5 has an INA226; the status log currently prints `BATT:0`. Wire it into the launcher status bar
      and the in-game overlay.
- [ ] **Backlight control** — expose brightness in the options menu (the LEDC backlight is already initialised).

## Emulation

- [ ] **Port another core** — NES (nofrendo) is the smallest step; SNES (snes9x) and Genesis (gwenesis) are in the tree already.
      Each needs a target wiring pass: display geometry, input mapping, audio rate, savestate paths.
- [ ] Frame-skip policy: expose the auto-frameskip threshold as a setting instead of a compile-time constant.
- [ ] Per-ROM settings persistence (scale, frameskip, audio volume).

## Launcher / UX

- [ ] Screenshots and a short capture in the README (nothing sells a port like seeing it run).
- [ ] Cover art and metadata polish (the SD-card layout already has a `romart` folder).
- [ ] Favourites / recently played.
- [ ] A settings screen that persists across reboots.

## Build & CI

- [ ] **GitHub Actions build check** using the official `espressif/idf` container — it can at least prove the tree compiles with
      `--target tab5 --no-networking` on every PR. This is the single biggest quality-of-life item for contributors.
- [ ] A tagged release workflow that attaches the merged `.img` to GitHub Releases.

## Docs

- [ ] Build walkthrough for Linux and Windows.
- [ ] Architecture notes: how the dynarec maps GBA memory, how the display task pipeline works, where the PSRAM budget goes.
- [ ] Translate README into more languages.

## Explicitly out of scope

- **Wireless anything.** The ESP32-P4 has no Wi-Fi and no Bluetooth. Not a limitation of this port — of the silicon.
- **Shipping ROMs.** Never.
