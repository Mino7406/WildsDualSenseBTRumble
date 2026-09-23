# Wilds DualSense BT Rumble

*[한글 설명 보기](README.ko.md) · [Nexus Mods page](https://www.nexusmods.com/monsterhunterwilds/mods/4944)*

Brings controller rumble back to **Monster Hunter Wilds** when a DualSense is connected over Bluetooth.

## The problem

Wilds drives a DualSense through the "advanced" haptic path. Those haptics reach the controller's voice coils as an audio stream over the USB audio endpoint the pad exposes when it is plugged in. **Over Bluetooth that endpoint does not exist, and Wilds has no ordinary rumble fallback** — so wireless play is completely silent. Most other games are unaffected because they send plain rumble, which Bluetooth carries fine.

## What this does

It reads the motor waveform Capcom already authored — intensity, duration, which motor, whether it fades out — and sends it to the pad over Bluetooth itself.

This is not an approximation. Every weapon and every situation uses the game's own vibration data, triggered by the game's own events. The mod only observes the game; it changes no behaviour and overwrites no values. It adds delivery over a path that was silent.

## Install

Grab a release and extract it into your Monster Hunter Wilds folder:

```
MonsterHunterWilds/reframework/autorun/WildsDualSenseBTRumble.lua
MonsterHunterWilds/reframework/plugins/WildsDualSenseBTRumble.dll
```

Requires [REFramework](https://github.com/praydog/REFramework) and a DualSense on Bluetooth. Nothing to run, no runtime to install.

## How it works

Two halves, because neither can do the job alone.

**`lua/WildsDualSenseBTRumble.lua`** hooks `ace.PadVibrationManager<app.cADVibration>.tryADVibration`. The game calls it with the ordinary motor preset *before* routing the request down the DualSense haptic path, so the preset is readable there without changing anything. Its `cMotorVibration` entries carry duration, motor, power and a fade flag. The mod replays them on its own clock, mixes whatever is active, and publishes the current per-motor level to a file.

**`plugin/plugin.cpp`** runs inside the game process, reads those levels, and writes DualSense Bluetooth output reports — report id `0x31`, 78 bytes, CRC32 seeded with `0xA2` — straight to the controller.

The plugin exists because REFramework's lua sandbox cannot reach a HID device, and the engine's own motor path (`via.hid.GamePadDevice.setMotorPower`) is routed into the same silent haptic path. Writing the reports ourselves is the only way through.

### Two details worth knowing

**Steam Input hides controllers from the game process.** It does not block opening the device — it makes the HID *queries* fail, returning `FALSE` from `HidD_GetAttributes` and `HidD_GetPreparsedData` even though the underlying call filled the struct correctly. The plugin therefore enumerates through `cfgmgr32` (SetupAPI is the one Steam filters), identifies the pad from its interface path, and writes reports without asking for capabilities it knows.

**Lua file IO is sandboxed to `reframework/data`.** Paths in the lua are bare names for that reason.

## Building

Needs a MinGW-w64 GCC toolchain. Visual Studio is not required — the plugin exposes a C ABI and passes no C++ objects across the boundary.

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
.\build.ps1
```

`build.ps1` fetches the REFramework plugin headers, compiles the DLL, and writes both release archives to `out/`. The English and Korean builds differ by exactly one line — `local LANGUAGE` in the lua — plus the bundled README.

`-static` matters: without it the DLL needs `libstdc++`, `libgcc` and `libwinpthread` alongside it. As built, it imports only `KERNEL32`, the UCRT `api-ms-win-crt-*` set and `SETUPAPI`.

Protocol details, measured tuning numbers and the open items live in [NOTES.md](NOTES.md).

## Settings

In the REFramework menu (Insert), under **Wilds DualSense BT Rumble**.

One control on top: **vibration strength**, as a percentage of what the game asks for. It defaults to 50%, which suits a DualSense — Capcom's levels were authored for the weighted motors in an ordinary gamepad, and voice coils render the same numbers considerably harder.

Under **Advanced settings**:

| | |
|---|---|
| Contrast (gamma) | pushes light rumble toward silence and keeps hits punchy |
| Gate | drops authored rumble weaker than this outright |
| LOW / HIGH | the low- and high-frequency motors, independently |

If it feels like a constant buzz rather than distinct hits, **raise the Gate** — it is the only control that reduces how much of the time the motors run. Turning the strength down instead squeezes everything into one narrow band and makes it worse. Measured in combat, moving the Gate from 0.15 to 0.30 cut the motors' running time from 38% to 21% while letting real hits land twice as hard.

## Limitations

- On a USB cable the mod stays dormant. The game's real haptics work there and are better than plain rumble.
- Adaptive triggers still do not work over Bluetooth. They need the same USB audio endpoint and cannot be fixed from here.
- Only a DualSense is looked for; other controllers are ignored.

## Credits

Protocol details follow the Linux [`hid-playstation`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c) driver. Plugin headers come from [REFramework](https://github.com/praydog/REFramework).

## License

[MIT](LICENSE)
