# Contributing

Thanks for helping Xbox 360 emulation run better on Mali.

## Reports (the most useful thing right now)

Only one phone has been measured in depth, so every report from a different device matters.

- **Device / game report:** [open one](../../issues/new?template=device-report.yml) with your phone, SoC/GPU, build, game, fps and an in-game screenshot.
- **Bug report:** [open one](../../issues/new?template=bug-report.yml). Add a logcat excerpt when the app crashes or shows black frames.
- **Questions and setups:** use [Discussions](../../discussions).

Tips for useful numbers: note whether the phone was charging and how warm it was. Heat and charging change fps a lot.

## Pull requests

- Keep each PR focused on one change, and explain what it fixes or speeds up.
- For performance changes, include before/after numbers from the same scene on the same device, with a similar starting temperature.
- Do not trade image quality for fps. Changes must be lossless or visually identical.
- Keep the shared core general. Per-game behavior belongs in `game_quirks.cc`, and only when a game really behaves differently.

## Never include

Games, BIOS files, keys or any copyrighted content.
