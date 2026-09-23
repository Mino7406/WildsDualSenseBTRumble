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

- Settings and the live levels share `WildsDualSenseBTRumble.txt`, every value
  under its own name. v1.0's `.json` is imported on first run and then deleted.
- The plugin's own `.log` is gone. It reports through REFramework's logger
  instead, and only on a change of state, so a healthy session writes nothing.
- Both leftovers are cleaned up on update. The lua removes the `.json` after
  importing it; the plugin deletes the `.log` at startup, and deletes the
  `.json` as a fallback - but only after it has seen a named state line, which
  proves the v1.1 lua already had its chance to import it.

Both halves were checked against each other offline, which covers the parsing but
not the game. **Smoke-test before publishing:** launch, open the menu, move the
strength slider, then confirm `WildsDualSenseBTRumble.txt` carries the new value
and a `state` line whose `sequence` climbs — and that rumble still arrives. If the
v1.0 settings came across, the old `.json` will have removed itself.

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

It checks that the lua's output parses, that a settings line is not mistaken for
levels, that a state line torn before `high=` is rejected rather than guessed at,
that the v1.0 format still reads, that the state line is still found in an
oversized file, and that out-of-range values are clamped. Run it before and after
touching the parser - it covers the half of this that needs no controller.

## Open items

### 1. Fade curves are linear; the game's are not

This is the one real known gap. `cMotorVibration` carries both
`_IsTimeAttenuation` and `_TimeAttenuationType`. Only the flag is read
(`lua/WildsDualSenseBTRumble.lua:119`), so every fading entry decays on a straight
line (`:207`):

```lua
local power = v.atten and (base * (1.0 - v.t / v.dur)) or base
```

`_TimeAttenuationType` is a `via.curve.EaseType`. Quest completion is where the
difference shows: Capcom authored a ~7 s entry there, and a linear ramp makes it
read as one long flat buzz instead of a decaying flourish.

Fix: dump the `EaseType` enum from the TDB, map its values to the matching curves,
and apply that instead of the linear term. Everything else in the pipeline already
carries the per-voice data needed.

### 2. Crashes seen 2026-09-19..22 — not caused by this mod

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

### 3. Release

- Nexus: EN zip as the main file, KR as the optional file. The two differ by one
  line of lua and the bundled README, both handled by `build.ps1`.
- A GitHub release tagged `v1.0` with both zips attached is worth doing once the
  Nexus page exists — deliberately not tagged yet, so the tag does not end up
  pointing at something that changed before it shipped.

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
