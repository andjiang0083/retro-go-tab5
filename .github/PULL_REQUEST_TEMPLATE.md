## What does this change?

<!-- One or two sentences. Link the issue it closes: Closes #N -->

## Why

<!-- The reason. If it is a display-path or timing change, explain the constraint you worked around. -->

## How was it verified?

- [ ] Built from a **clean** tree with `--target tab5 --no-networking`
- [ ] Flashed to a real Tab5 and exercised on hardware
- [ ] Launcher boots and a GBA game runs at 59-60 logical fps with audio
- [ ] If the display path was touched: played 10+ minutes with no tearing and no freeze
- [ ] If this is a risky change: it is behind a runtime switch, and the switch is documented

## Measurements (if performance-related)

<!-- Before/after. The FPS line format is: FPS: total (skipped+partial+drawn), BUSY: n% -->

| | Before | After |
|---|---|---|
| Drawn frames/sec | | |
| BUSY | | |

## Notes for reviewers

<!-- Anything you are unsure about, or a follow-up you deliberately left out. -->
