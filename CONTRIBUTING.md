# Contributing

Thanks for considering it. This is a single-board port maintained by people who own the hardware, so the most useful
contributions are the ones that can be verified on a real Tab5.

## Ways to help

- **Pick something from [ROADMAP.md](ROADMAP.md)** and open an issue saying you are taking it, so nobody duplicates work.
- **Fix a bug** — please include the evidence described below.
- **Improve docs** — screenshots, a build walkthrough for Windows/Linux, translations.
- **Port another core** (NES, SNES, Genesis, ...) — the retro-go tree already carries them; they need a target wiring pass.

## House rules (they come from real failures, not taste)

1. **One visible change per flash.** Flash it, look at the screen, then continue. Multiple changes per flash make a regression
   impossible to attribute.
2. **No serial-console debug loops.** Opening the USB-CDC port resets the device (see BUILDING.md). Verify visually, or read
   `/crash.log` from the SD card.
3. **Anything touching the display path or PSRAM bandwidth ships behind a runtime switch** — an opt-in file on the SD card, read
   at startup. A bad experiment must be rollback-able without reflashing. This is not optional: on this board, display-path
   changes can wedge the panel in a way that only a power cycle fixes.
4. **Start from a known-good baseline; prefer small targeted changes over rewrites.** A measured 15% improvement that is
   verifiable beats a rewrite that might be 40%.

## Building and testing

See [BUILDING.md](BUILDING.md). Before opening a PR:

- [ ] The build completes with `--no-networking` from a **clean** tree (not just incrementally).
- [ ] The device boots, the launcher works, a GBA game runs at 59-60 logical fps with audio.
- [ ] If you touched the display path: no tearing, no freeze over a 10+ minute session.
- [ ] If you touched anything risky: it is behind a runtime switch, and you documented the switch.

## Reporting a bug

Use the issue template, and include:

- **What you saw** — screen behaviour, and the `FPS:`/`BUSY:` line if you have it.
- **Build info** — the `Build info:` string printed at boot (target, version, SDK, chip).
- **`/crash.log`** — pulled from the SD card, if one was written. Include the file's date.
- **Whether it recovered** — did it reboot itself, or did it need a power cycle? That single detail separates a software panic
  from a hardware-level hang, and they have completely different causes.

## Commit and PR style

- Conventional-commit prefixes (`feat:`, `fix:`, `docs:`, `refactor:`, `perf:`, `chore:`) are appreciated.
- English or 中文 are both fine — use whichever expresses the change more precisely.
- Explain *why* in the body, especially for anything touching the display path or timing.

## Code style

Match the surrounding code (upstream retro-go style: 4-space indent, brace on its own line for functions, `rg_`-prefixed
framework symbols). Port-specific additions should be marked with a short comment explaining the board constraint they work around —
the next person needs to know which lines are upstream and which are Tab5-specific.
