# Developer notes

The README is for people installing the mod. This is for whoever picks the code
back up — what is known, what is done, and what is left.

## State

v1.0 is complete and tested in combat: all weapons, all situations, because the
waveform is Capcom's own rather than anything hand-authored here. The lua hook is
read-only, the plugin only writes to the pad, and neither touches game state.

Defaults after tuning: strength 50%, contrast (gamma) 1.90, gate 0.15.

**v1.1 is housekeeping and has not been run in the game yet.** Nothing about the
rumble itself changed. What changed is what the mod leaves in `reframework/data`,
three files down to one:

- Settings and the live levels share `WildsDualSenseBTRumble.json` — `settings`
  for what the menu saved, `state` for the levels the plugin reads. That is the
  file v1.0 already kept settings in, so there is nothing to migrate: the loader
  takes them from `settings` if that key is there and from the top level if not.
- `state` is written alone on one line, after the settings. lua rewrites by
  truncating, so a reader only ever catches a prefix; the settings sit clear of
  that window, and a torn `state` line fails the all-three-fields test instead of
  being half-read. The plugin matches `"low"` with its quotes so it cannot pick
  up `"lowScale"` from the settings above — there is a test for exactly that.
- The plugin's own `.log` is gone. It reports through REFramework's logger
  instead, and only on a change of state, so a healthy session writes nothing.
- v1.0's leftovers are deleted on update: the lua removes the orphaned `.txt`,
  and the plugin removes both it and the `.log` when the worker starts.
- Strict json is all-or-nothing, so a truncated file would otherwise reset every
  setting. `scavengeSettings()` is the fallback: if `json.load_file` returns
  nothing, the values are scraped off the lines with a pattern.

Both halves were checked against each other offline, which covers the parsing but
not the game. **Smoke-test before publishing:** launch, open the menu, move the
strength slider, then confirm `WildsDualSenseBTRumble.json` carries the new value
and a `state` whose `sequence` climbs — and that rumble still arrives. The old
`.txt` and `.log` should be gone from `reframework/data`.

## Picking this up on another machine

```powershell
git clone https://github.com/Mino7406/WildsDualSenseBTRumble
winget install BrechtSanders.WinLibs.POSIX.UCRT
.\build.ps1
```

That is the whole setup. `out/` then holds the DLL and both release zips. The
REFramework headers are fetched by the script; `out/` and `plugin/include/` are
untracked on purpose, so a fresh clone is the full source of truth. Verified from
scratch on 2026-09-23.

Code, docs and packaging can be done anywhere. **Anything that changes how the
rumble feels has to be confirmed on real hardware** — the game, REFramework and a
DualSense paired over Bluetooth. There is no way to judge gamma or gate by reading
the diff.

To install a test build:

```
out\WildsDualSenseBTRumble.dll  ->  MonsterHunterWilds\reframework\plugins\
lua\WildsDualSenseBTRumble.lua  ->  MonsterHunterWilds\reframework\autorun\
```

The plugin reports through REFramework's own logger, so look in
`re2_framework_log.txt` and search for `WildsDualSenseBTRumble`. It says whether a
pad was found, opened, and over which transport. A working session logs nothing,
which is itself the answer when the file is silent and the rumble works.

## Testing what can be tested off the hardware

`plugin/parse_test.cpp` compiles the plugin itself and runs its reader over the
exact bytes the lua writes, so the two halves cannot drift apart unnoticed:

```powershell
g++ -std=c++17 -O1 -I plugin -I plugin/include -o parse_test.exe plugin/parse_test.cpp -lsetupapi
.\parse_test.exe
```

Nine checks: that the lua's output parses, that `"lowScale"` is not read as
`"low"`, that a settings-only file yields nothing, that a `state` torn before
`"high"` is rejected rather than guessed at, that it is still found in an
oversized file, that out-of-range values are clamped, and that a v1.0 settings
file gives no levels. Run it before and after touching the parser - it covers the
half of this that needs no controller.

## Open items

### 1. Fixed: rumble lingering through map transitions / loading screens (2026-09-26)

Nexus feedback (Helcyin, 4944): random, sometimes-long rumble during map
transitions and loading screens. Root cause was `delta()`
(`lua/WildsDualSenseBTRumble.lua`): on a real stall - engine hitch or a loading
screen where `on_frame` gaps by more than 0.25s of `os.clock()` - it substituted
an assumed `1/60`s instead of the real gap. Any voice active when the stall began
barely decayed (`v.t` advanced by the assumed tiny amount, not the real elapsed
time), so it kept buzzing for the whole stall/load and then continued afterward
for its now-almost-full remaining duration. Duration was "random" because it
tracked how long that particular load happened to take, not anything about the
preset.

Fix: `delta()` now also returns whether the gap was a stall; `on_frame` clears
`voices` outright when it is, instead of letting them decay in slow motion.
**Not yet confirmed on hardware** - needs a map transition with the pad connected
to be sure it kills the phantom rumble without cutting real combat rumble that
happens to land on an ordinary hitch.

### 2. Fade curves are linear; the game's are not

This is the one real known gap. `cMotorVibration` carries both
`_IsTimeAttenuation` and `_TimeAttenuationType`. Only the flag is read
(`lua/WildsDualSenseBTRumble.lua:155`), so every fading entry decays on a straight
line (`:276`):

```lua
local power = v.atten and (base * (1.0 - v.t / v.dur)) or base
```

`_TimeAttenuationType` is a `via.curve.EaseType`. Quest completion is where the
difference shows: Capcom authored a ~7 s entry there, and a linear ramp makes it
read as one long flat buzz instead of a decaying flourish.

Fix: dump the `EaseType` enum from the TDB, map its values to the matching curves,
and apply that instead of the linear term. Everything else in the pipeline already
carries the per-voice data needed.

### 3. Crashes seen 2026-09-19..22 — not caused by this mod

Established from WER `Report.wer`, which carries the faulting offset and the full
`LoadedModule[]` list — far better evidence than the Application event log, whose
retention window made it look like there had been only one crash when there were
six. The loaded modules pointed at a February-2026 NVIDIA Streamline (DLSS / Frame
Generation) stack running against an August-2026 game build.

Remediated by verifying files through Steam and then copying the 12 restored DLLs
from the game root into `_storage_`, which is where the game actually loads them
from — Steam's verify does not touch that folder.

**Outcome not yet confirmed by play time.** If it recurs, a faulting offset of
`0b009a2a` again means the same bug; the next thing to look at is the ReShade
preset and shaders changed 2026-09-19/20.

### 4. Release

- The Nexus page is https://www.nexusmods.com/monsterhunterwilds/mods/4944. EN zip
  is the main file, KR the optional one. The two differ by one line of lua and the
  bundled README, both handled by `build.ps1`.
- GitHub carries tags `v1.0` and `v1.1`. Publishing a release is a separate step
  from tagging, and is deliberately left until a build has been run in the game.

## Reference: things that cost time to find out

### DualSense Bluetooth output report

| | |
|---|---|
| Report id | `0x31` (byte 0) |
| Sequence | byte 1, `(n & 0x0F) << 4` |
| Tag | byte 2 = `0x10` |
| valid_flag0 | byte 3 = `0x03` (HAPTICS_SELECT \| COMPATIBLE_VIBRATION) |
| Motors | byte 5 = right, byte 6 = left |
| CRC32 | bytes 74–77, poly `0xEDB88320`, init `0xFFFFFFFF`, seed byte `0xA2` prepended, result inverted, computed over the first 74 bytes |

The report core is 78 bytes, but Windows reports `OutputReportByteLength` = **547**
for a DualSense on Bluetooth, and the write must be that long. USB is 64.
`WriteFile` on the interrupt channel works; `HidD_SetOutputReport` silently does
nothing.

### Steam Input

It does not block `CreateFileW` — it makes the HID *queries* fail.
`HidD_GetAttributes` and `HidD_GetPreparsedData` return `FALSE` while having filled
the struct correctly. So VID/PID cannot be used to identify the pad from inside the
game process. Identify it from the interface path instead (`054c` + `0ce6`), and
detect Bluetooth by `{00001124-0000-1000-8000-00805f9b34fb}` in the same string.

Enumerate through **cfgmgr32**, not SetupAPI: Steam hooks SetupAPI and leaves
cfgmgr32 alone.

Also worth knowing: Wilds is a native Steam Input API game, so Steam inserts no
emulated XInput pad. Advice that works for Cyberpunk 2077 does not apply here.

### RE Engine side

```
app.AppPadVibrationManager.requestVibration(Guid, …)
  -> ace.PadVibrationManager`1<app.cADVibration>.tryADVibration   <- hooked here
       ace.user_data.PadVibrationPresetList.cPadVibrationPreset
         ._MotorVibration[] { _Time, _MotorType, _Power,
                              _IsTimeAttenuation, _TimeAttenuationType }
```

`_AllPresetDict` holds 348 presets. `ace.cPadInfo.VIBRATION_MOTOR_TYPE` is LOW,
HIGH, LTRIGGER, RTRIGGER — the trigger motors are decoded but silent over
Bluetooth.

Hooking `ace.cPadInfo.VibrationJob.update` **crashes the game** — it was tried
twice, and the second time the game would not launch until the script was deleted.
Stay out of that region. Keep hook counts to one or two.

REFramework's lua has `sdk.hook`, `find_type_definition`, `get_managed_singleton`,
`get_native_singleton`, `call_native_func`, `to_managed_object`, `to_float`,
`to_int64` — but no `get_tdb`, `get_managed_singletons` or `get_native_singletons`.
File IO is sandboxed to `reframework/data`, which is why paths in the lua are bare
names.

### Tuning numbers (measured, not guessed)

Motor duty cycle in combat: **46.5% → 38.1% → 21.1%** as the gate went 0.10 → 0.15
→ 0.30. Gate — not strength — is what controls how busy the rumble feels. Lowering
strength squeezes everything into one narrow band and makes it worse.

`minOutput` is a hard floor and must stay near `0.004`. At `0.02` it created a dead
zone: with gamma 1.9, a 0.35-power preset needed strength ≥ 15% just to clear the
floor, so the bottom of the slider went silent.
