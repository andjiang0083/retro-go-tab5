# Roadmap

**English** · [中文](ROADMAP_CN.md)

Roughly ordered. Items are sized so a newcomer can pick one up. **Comment on the issue (or open one) before starting**, so
work does not collide.

*Updated for v0.4.7 — the port now covers 11 consoles, four touch skins and per-console controls.
Items that shipped are gone from this list; see [CHANGELOG.md](CHANGELOG.md).*

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

## Emulation — depth over breadth

Eleven consoles are wired up (GBA, GB, GBC, NES, SNES, SMS, Game Gear, ColecoVision, PC Engine, Lynx, Game & Watch).
The next wins are in quality, not in adding more:

- [ ] **Per-console accuracy pass** — play a set of known-demanding titles per core and record what breaks. Some cores
      (e.g. SNES) have never had their speed measured on this board; publish the numbers.
- [ ] Frame-skip policy: expose the auto-frameskip threshold as a setting instead of a compile-time constant.
- [ ] Per-ROM settings persistence (scale, frameskip, audio volume).
- [ ] Port another core from upstream retro-go (MD / MSX / ...) — each needs a target wiring pass: display geometry,
      input mapping, audio rate, savestate paths. Follow the per-console controls rule: map **what the real controller has**.

## Launcher / UX

- [ ] Cover art and metadata polish (the SD-card layout already has a `romart` folder).
- [ ] Favourites / recently played.
- [ ] A settings screen that persists across reboots.
- [ ] **Backlight control** — expose brightness in the options menu (the LEDC backlight is already initialised).

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
