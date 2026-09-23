<div align="center">

# Wilds DualSense BT Rumble

Brings controller rumble back to **Monster Hunter Wilds** when a DualSense is connected over Bluetooth. Built on [REFramework](https://github.com/praydog/REFramework).

[![Platform](https://img.shields.io/badge/platform-Windows-0078D6?style=flat-square&logo=windows&logoColor=white)]()
[![REFramework](https://img.shields.io/badge/REFramework-plugin-5865F2?style=flat-square)](https://github.com/praydog/REFramework)
[![Nexus Mods](https://img.shields.io/badge/Nexus%20Mods-download-D98F40?style=flat-square)](https://www.nexusmods.com/monsterhunterwilds/mods/4944)
[![License: MIT](https://img.shields.io/badge/license-MIT-4c1?style=flat-square)](LICENSE)
![language](https://img.shields.io/badge/docs-EN%20%7C%20KR-blue?style=flat-square)

*[한글 설명 보기](README.ko.md) · [Nexus Mods 페이지](https://www.nexusmods.com/monsterhunterwilds/mods/4944)*

</div>

## The problem

Wilds drives a DualSense through the "advanced" haptic path. Those haptics reach the controller's voice coils as an audio stream over the USB audio endpoint the pad exposes when it is plugged in. **Over Bluetooth that endpoint does not exist, and Wilds has no ordinary rumble fallback** — so wireless play is completely silent. Most other games are unaffected because they send plain rumble, which Bluetooth carries fine.

---

## What this does

It reads the motor waveform Capcom already authored — intensity, duration, which motor, whether it fades out — and sends it to the pad over Bluetooth itself.

This is not an approximation. Every weapon and every situation uses the game's own vibration data, triggered by the game's own events. The mod only observes the game; it changes no behaviour and overwrites no values. It adds delivery over a path that was silent.

---

## Install

Grab a release from [Nexus Mods](https://www.nexusmods.com/monsterhunterwilds/mods/4944) or the [GitHub releases](../../releases) and extract it into your Monster Hunter Wilds folder so you end up with:

```
MonsterHunterWilds/
└─ reframework/
   ├─ autorun/
   │  └─ WildsDualSenseBTRumble.lua
   └─ plugins/
      └─ WildsDualSenseBTRumble.dll
```

> [!NOTE]
> Requires [REFramework](https://github.com/praydog/REFramework) and a DualSense on Bluetooth. Nothing to run, no runtime to install — both files load automatically when the game starts.

---

## How it works

Two halves, because neither can do the job alone.

**`lua/WildsDualSenseBTRumble.lua`** hooks `ace.PadVibrationManager<app.cADVibration>.tryADVibration`. The game calls it with the ordinary motor preset *before* routing the request down the DualSense haptic path, so the preset is readable there without changing anything. Its `cMotorVibration` entries carry duration, motor, power and a fade flag. The mod replays them on its own clock, mixes whatever is active, and publishes the current per-motor level to a file.

**`plugin/plugin.cpp`** runs inside the game process, reads those levels, and writes DualSense Bluetooth output reports — report id `0x31`, 78 bytes, CRC32 seeded with `0xA2` — straight to the controller.

The plugin exists because REFramework's lua sandbox cannot reach a HID device, and the engine's own motor path (`via.hid.GamePadDevice.setMotorPower`) is routed into the same silent haptic path. Writing the reports ourselves is the only way through.

<details>
<summary>🧩 <b>Two details worth knowing</b></summary>

**Steam Input hides controllers from the game process.** It does not block opening the device — it makes the HID *queries* fail, returning `FALSE` from `HidD_GetAttributes` and `HidD_GetPreparsedData` even though the underlying call filled the struct correctly. The plugin therefore enumerates through `cfgmgr32` (SetupAPI is the one Steam filters), identifies the pad from its interface path, and writes reports without asking for capabilities it knows.

**Lua file IO is sandboxed to `reframework/data`.** Paths in the lua are bare names for that reason.

**The mod leaves one file behind, not three.** Settings and the live motor level share `reframework/data/WildsDualSenseBTRumble.txt`, every value written under its own name so neither half can be read as the other. Settings come first and the state line last: lua rewrites the file by truncating it, so a reader that catches it mid-write sees a prefix, and naming every field means a torn state line is rejected rather than half-read. The plugin keeps no log of its own - it reports through REFramework's logger, and only when something changes.

</details>

---

## Settings

In the REFramework menu (Insert), under **Wilds DualSense BT Rumble**.

One control on top: **vibration strength**, as a percentage of what the game asks for. It defaults to 50%, which suits a DualSense — Capcom's levels were authored for the weighted motors in an ordinary gamepad, and voice coils render the same numbers considerably harder.

Under **Advanced settings**:

| | |
|---|---|
| Contrast (gamma) | pushes light rumble toward silence and keeps hits punchy |
| Gate | drops authored rumble weaker than this outright |
| LOW / HIGH | the low- and high-frequency motors, independently |

> [!TIP]
> If it feels like a constant buzz rather than distinct hits, **raise the Gate** — it is the only control that reduces how much of the time the motors run. Turning the strength down instead squeezes everything into one narrow band and makes it worse. Measured in combat, moving the Gate from 0.15 to 0.30 cut the motors' running time from 38% to 21% while letting real hits land twice as hard.

---

## Limitations

- On a USB cable the mod stays dormant. The game's real haptics work there and are better than plain rumble.
- Adaptive triggers still do not work over Bluetooth. They need the same USB audio endpoint and cannot be fixed from here.
- Only a DualSense is looked for; other controllers are ignored.

---

## Credits

Protocol details follow the Linux [`hid-playstation`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c) driver. Plugin headers come from [REFramework](https://github.com/praydog/REFramework).

## License

[MIT](LICENSE)
