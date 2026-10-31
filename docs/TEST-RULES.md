# Test rules (incontrovertible)

Every rule comes from a mistake already made. Before every run and every queue, check all of them.
The program `xdtest.py` (test folder `lab/test/`) applies them by itself: always use it. Usage: `python3 xdtest.py TAG qNNN BO|DANTE|REACH [Tabella.chiave=valore ...] [--prof] [--secs N]` → standard line in `TESTS.tsv` and `NIGHT.tsv`.

## Before working
1. **No work already done.** Search LAB-NOTEBOOK.md and NIGHT.tsv on the topic before starting.
2. **Only the latest build.** All queues point to the latest qNNN. The APK must exist before the queue starts.

## Queues
3. **Never rewrite a queue file that has already started** (`cat >` or `sed -i` change the file under bash). Only in-place edits are allowed, at the same length or after the current position, or a new file.
4. **Never force-stop or kill blindly.** First `pgrep` the runners. Stop only the specific runner (SIGINT), never while another queue is launching.
5. **Function names in scripts do not cover commands** (`tr`, `cut`, `sort`...).

## Settings
6. **String cvars must be in quotes** (`GPU.x='/percorso'`). Without quotes the TOML is not read and the emulator dies at startup.
7. **A specific file, never `head -1`** (e.g. `default.image.bin`, not the first `*.image.bin`).

## Measurements
8. **`xe.log` accumulates across runs.** Read only the new lines, after the saved start line.
9. **Controls have no profiler.** Profiles are taken in a separate run, with tag `-prof`, which is not valid as a comparison.
10. **Same scene:** compare by restarting from the savestate, not timed from the ISO (you risk landing in the loading screen).
11. **One run is a clue, not a verdict.** The verdict comes from an ABAB at the same starting temperature, or from objective counters (misses, hits, calls).
12. **Thermal:** start at ≤38 °C (Reach from ISO ≤37). The stop at 46.5 °C is not changed. Record the starting temperature and the charger status.

## Code
13. **Delicate paths (savestate, teardown):** before changing them, think through every way they can fail. A restore without the game frozen can hang the teardown.
14. **Diagnostics are removed** once their question has been answered.
15. **`pgrep -f NOME` inside a `zsh -c`/`bash -c` command finds itself** (the command line contains NOME) → an `until ! pgrep -f build-qNNN.py` never ends. Use `pgrep -f "[b]uild-qNNN.py"` or do it inside a script/Python.
16. **The input file must be passed with an absolute path.** The runner works in another folder: a relative path was not found and the run started without input, stuck on the title screen (REACH9-13, 24/10). Now `reach-save.py`/`reach-shot.py` resolve it and `pkg_run_c.py` stops if `XD_GAME_INPUTS_FILE` is missing.
17. **Savestates and measurements only in verified gameplay.** Never trust a fixed time: check draws/frame at save time (Reach: title 320-440, loading 2-60, gameplay 1000-3500 → threshold 900) and the screenshot. Reach idling on the title starts a demo by itself: it looks like gameplay footage but is not.
18. **A trigger on the phone log must see the log of the CURRENT run.** The previous run's `xe.log` stays until the new process rewrites it: a "2xxx draw" threshold fires immediately on the old one (REACH53 audit at frame 0, REACH54 armed at boot, 25/10). First wait for the old `RuntimeState: restore OK` to disappear, then search for it again and read only the GuestFps after it (queue-1025-j.sh `ingame`).
19. **Live cvars go in `compose/ui-live.toml`.** The UI forces `live_cvar_file` to that file: another path passed as a cvar is ignored without errors (REACH54/BO55 empty). Check the `LiveCvar: ... applied:` line in the log before trusting the run.
