Wilds DualSense BT Rumble  v1.0
===============================

Brings controller rumble back to Monster Hunter Wilds when a DualSense is
connected over Bluetooth.


WHY IT IS NEEDED
----------------
Wilds drives a DualSense through the "advanced" haptic path. Those haptics reach
the controller's voice coils as an audio stream over the USB audio endpoint the
pad exposes when it is plugged in. Over Bluetooth that endpoint does not exist,
and Wilds has no ordinary rumble fallback, so wireless play is completely silent.
Most other games are unaffected because they send plain rumble, which Bluetooth
carries fine.

This mod reads the motor waveform Capcom already authored - intensity, duration,
which motor, fade curve - and sends it to the pad over Bluetooth itself. It is
not an approximation: every weapon and every situation uses the game's own
vibration data, triggered by the game's own events.


REQUIREMENTS
------------
- Monster Hunter Wilds on PC
- REFramework installed
- A DualSense (PS5) controller connected over Bluetooth


INSTALL
-------
Extract this archive into your Monster Hunter Wilds folder, so that you end up
with:

    MonsterHunterWilds/reframework/autorun/WildsDualSenseBTRumble.lua
    MonsterHunterWilds/reframework/plugins/WildsDualSenseBTRumble.dll

Both files load automatically when the game starts. There is nothing to run and
no runtime to install.


SETTINGS
--------
Press Insert to open the REFramework menu, then expand
"Wilds DualSense BT Rumble".

There is one control on top: Vibration strength, as a percentage of what the
game asks for. It defaults to 50%, which suits a DualSense - the levels Capcom
authored were meant for the weighted motors in an ordinary gamepad, and the
voice coils in a DualSense render the same numbers considerably harder. Turn it
far enough down and the faintest rumble stops coming through at all, which is
the hardware running out of resolution rather than a fault.

Everything else lives under "Advanced settings":

    Contrast (gamma)   pushes light rumble toward silence and keeps hits punchy
    Gate               drops authored rumble weaker than this outright
    LOW / HIGH         the low- and high-frequency motors, independently
    Reset              back to the defaults
    Test buzz          fires a sample pulse

Defaults are 50%, Contrast 1.90, Gate 0.15. Settings are saved to
reframework/data/WildsDualSenseBTRumble.json and survive restarts.

If it feels like a constant buzz rather than distinct hits, raise the Gate -
that is the only control that reduces how much of the time the motors run.
Turning the strength down instead squeezes everything into one narrow band and
makes it worse. Measured in combat, moving the Gate from 0.15 to 0.30 cut the
motors' running time from 38% to 21% while letting real hits land twice as
hard, at the cost of the lightest feedback. Try 0.25-0.35 if the default feels
busy.


NOTES
-----
- On a USB cable the mod stays dormant, because the game's real haptics already
  work there and are better than plain rumble.
- It only observes the game. No game behaviour is changed and no values are
  overwritten; the mod only adds delivery over a path that was silent.
- Steam Input hides controllers from the game process, so the plugin identifies
  the pad by its device path and writes reports without the HID capability
  queries Steam blocks.
- Adaptive triggers still do not work over Bluetooth. That needs the same USB
  audio endpoint and cannot be fixed from here.
- Other controllers are ignored; it looks specifically for a DualSense.
- Troubleshooting: the plugin writes
  reframework/data/WildsDualSenseBTRumble.log, which says whether it found and
  opened the pad.


UNINSTALL
---------
Delete the two files. Optionally also delete WildsDualSenseBTRumble.txt,
WildsDualSenseBTRumble.json and WildsDualSenseBTRumble.log from
reframework/data.


KOREAN VERSION
--------------
A build with a Korean menu is available as a separate file. The two are
identical apart from the language the UI is drawn in.


HOW IT WORKS
------------
The lua part hooks ace.PadVibrationManager<app.cADVibration>.tryADVibration,
which the game calls with the ordinary motor preset before routing the request
to the DualSense haptic path. The preset's cMotorVibration entries carry the
duration, motor, power and fade flag. The mod replays them on its own clock,
mixes the active ones, and publishes the current levels.

The plugin runs inside the game process, reads those levels, and writes DualSense
Bluetooth output reports (report id 0x31, CRC32 with the 0xA2 seed) directly to
the controller.
