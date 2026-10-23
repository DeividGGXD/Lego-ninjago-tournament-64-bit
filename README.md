# LEGO Ninjago: Tournament on modern Android

LEGO Ninjago: Tournament (TT Fusion, 2015–2016) was pulled from Google Play years ago. It was only
ever built for 32-bit ARM, so it won't even install on most phones made since 2023: their
processors can't run 32-bit code at all.

This project gets it running again on today's 64-bit phones, with graphics, music and sound:

- [**THUMB**](https://github.com/whodfisbane/thumb) translates the game's 32-bit code to 64-bit as
  it runs.
- Two small libraries from this repository fill in what THUMB doesn't cover yet: **sound** (the game
  crashes at startup without it) and **3D textures** (without them the world renders black).
- A script fixes the problems found in the copies of the game data that are going around.

> **No game files are included.** You need your own copy of the APK and its OBB data file.

<!-- Add a gameplay screenshot here, e.g. docs/screenshot.png -->

## Status

| | |
|---|---|
| Starts, menus, character select, level select | ✅ |
| 3D world (textures decoded in software where needed) | ✅ |
| Music and sound effects | ✅ |
| Touch controls | ✅ |
| Save games persisting between launches | not tested yet |

Tested on a **vivo X200 Pro** (MediaTek Dimensity 9400, Mali-G925 GPU, Android 16), a phone with no
32-bit support. Reports from other phones are very welcome (see [Reporting problems](#reporting-problems)).

## What you need

**Phone**
- Android 10 or newer, 64-bit ARM. That covers nearly every phone from the last several years,
  including 64-bit-only ones (Pixel 7 and newer, Dimensity 9200 and newer, Snapdragon 8 Gen 3 and
  newer, and so on).
- About 3 GB of free space while installing (the data file is about 1 GB).

**Computer, once.** Windows, macOS or Linux with [Python](https://www.python.org/downloads/) 3.8 or
newer, to prepare the files. Nothing else needs installing.

**The game files** (your own copy):

| File | Tested with | Notes |
|---|---|---|
| APK | version **1.05.3.970** (version code 1055), SHA-256 `ec742c50…bfac91c` | The Android 14+ fixed build from [kurrukado/Ninjago-Game](https://github.com/kurrukado/Ninjago-Game). Other versions are untested. |
| OBB | `main.1057.com.lego.ninjago.toe.obb`, 897,622,426 bytes, SHA-256 `6a47cca0…a9e5f8fd` | A re-upload with broken packing; `prepare.py` fixes it. Other copies should work too. |

<!-- Links to the files go here.
APK:
OBB:
-->

The OBB doesn't have to have the same build number as the APK: `prepare.py` names it for the APK.

Back when the game was on Google Play, every GPU family got its own OBB with different texture
compression. Any of them should now work, except PowerVR:

| OBB edition (texture format) | Originally for | Status |
|---|---|---|
| ATC | Qualcomm Adreno | ✅ tested; decoded in software on other GPUs |
| ETC1 | every GPU | should work as-is (untested) |
| DXT / S3TC | Nvidia Tegra | decoded in software (untested) |
| PVRTC | PowerVR | ❌ not supported yet: the world stays black |

## Installing

1. **Install THUMB** on the phone: download `THUMB-0.2.0.apk` from the
   [THUMB releases](https://github.com/whodfisbane/thumb/releases) and install it. This project was
   tested with THUMB 0.2.0.

2. **Prepare the files** on the computer. Download this repository (green **Code** button →
   **Download ZIP**), unzip it, open a terminal in the folder and run:

   ```
   python prepare.py --apk "path/to/your.apk" --obb "path/to/your.obb"
   ```

   (On some systems the command is `python3`.) This writes two files to the `output` folder:
   - `NinjagoTournament-THUMB-input.apk`: your APK with the sound and graphics libraries added
   - `main.<version>.com.lego.ninjago.toe.obb`: the data file, fixed and correctly named

3. **Copy both files to the phone**, for example into `Download`.

4. In **THUMB**, tap **Add app** and pick `NinjagoTournament-THUMB-input.apk`. THUMB Doctor should
   say **✅ Should work**. Keep the default options and tap **Patch & install**.

5. When THUMB asks for the **data file (OBB)**, pick the prepared `main.….obb`. If it doesn't ask,
   open THUMB's **Patched** list and add it there. THUMB hands it to the game on the first launch,
   which takes a few seconds.

6. **Launch the game.** If Android warns that the app was built for an older version of Android,
   tap OK.

Once it works you can delete the copies in `Download`.

## Optional: cheats

Add `--mods` when preparing the APK:

```
python prepare.py --apk "path/to/your.apk" --obb "path/to/your.obb" --mods
```

This writes `NinjagoTournament-THUMB-input-mods.apk`, which you install the same way. It switches on
the developer cheats TT left in the game, and keeps your money topped up:

- studs and tokens stay at 10,000,000: whenever they drop below 5,000,000 they're refilled
- every character, super move, level, extra and red brick unlocked
- TT's "complete everything" runs once per launch, which marks all levels complete

The last point changes your save for good, so keep a non-mods install if you want to play through
normally. `mods.log` (next to the other logs) shows whether each cheat was found. Not tested on a
phone yet.

## Troubleshooting

| What you see | What to do |
|---|---|
| "Download failed because the resources could not be found" | The game can't find its OBB. Add it again in THUMB's **Patched** list, and make sure it came out of `prepare.py` (the name has to match the APK's version). |
| The game closes right after the black loading screen | The sound library is missing: patch `NinjagoTournament-THUMB-input.apk`, not the original APK. |
| Menus work but the 3D world is black | The texture format isn't supported (PowerVR edition?). Check `graphics.log` (see below). |
| No music, but sound effects work | The OBB wasn't prepared with `prepare.py`. |
| Progress or OBB gone after reinstalling | Uninstalling deletes both. To update, re-patch through THUMB instead of uninstalling. |

### Reporting problems

The libraries write logs on the phone, in `Android/data/com.lego.ninjago.toe/files/`:

- `audio.log`: sound engine
- `graphics.log`: GPU name, texture formats, shader errors

Please include your phone model, Android version and these two files when you report a problem. On
some phones (vivo, for one) Android's system log drops the game's messages, which is why these
files exist.

## How it works

The game's engine is native ARMv7 code. There is no source for it, so it can't be recompiled for
64-bit. Instead, THUMB translates that code as it runs. The game's Java code, graphics and sound run
natively.

THUMB covered 211 of the 219 system functions the engine needs. The missing 8 were the OpenSL ES
sound API and one asset function, and the game crashes the moment it starts its sound engine. On top
of that, the OBB copies going around had two packing bugs, and they're the Adreno-only texture
edition, which other GPUs can't display.

This repository fills those gaps with two small 32-bit libraries that THUMB loads next to the game,
plus a script that fixes the data. The full story, with the details, is in
[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

## Building the libraries yourself

Prebuilt libraries are in `prebuilt/armeabi-v7a/`. To rebuild them you need the
[Android NDK](https://developer.android.com/ndk/downloads) (r25 or newer):

```
python build.py
```

Sources are in `src/`:

| File | Becomes | What it is |
|---|---|---|
| `slaudio.c` | `libOpenSLES.so` | the OpenSL ES subset the game uses, mixed in software and played through `AudioTrack` |
| `glfix.c` | `libGLESv2.so` | software decoding of ATC and DXT textures, plus shader and texture diagnostics |
| `mods.c` | `libGLESv2.so` (mods variant only) | the optional cheats |
| `dbglog.h` | | logging to logcat and to a file |
| `minimp3.h` | | MP3 decoder ([lieff/minimp3](https://github.com/lieff/minimp3), CC0) |

## Credits

- [**THUMB**](https://github.com/whodfisbane/thumb) by whodfisbane (GPL-3.0): the ARM32 → ARM64
  translator everything here runs on, built on [dynarmic](https://github.com/azahar-emu/dynarmic).
- [**minimp3**](https://github.com/lieff/minimp3) by lieff (CC0): MP3 decoding for the music.
- [**bimg**](https://github.com/bkaradzic/bimg) by Branimir Karadžić: reference for the ATC block
  layout.
- [**kurrukado/Ninjago-Game**](https://github.com/kurrukado/Ninjago-Game): earlier Android 14+ fixes
  for the game's Java side; the APK this was tested with.

## Legal

This is a non-commercial fan preservation project. It is not affiliated with or endorsed by the LEGO
Group, Warner Bros. Games or TT Games. LEGO and NINJAGO are trademarks of the LEGO Group. The game
belongs to its owners: this repository contains none of its code or data, and you need your own
copy. Like THUMB's author asks, please don't share patched APKs.

The code in this repository is released under the [MIT License](LICENSE); `src/minimp3.h` is CC0.
