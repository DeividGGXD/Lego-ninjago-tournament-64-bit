# How it works

This is the technical story of getting LEGO Ninjago: Tournament, a 32-bit ARM game from 2015/2016,
running on a phone that can't execute 32-bit code at all. It's written for developers who want to
check the work, port another game the same way, or take it further.

## 1. Why the game stopped working

The game ships one native library, `libLEGO_M1.so`, built only for `armeabi-v7a` and `x86`. It
holds TT Fusion's engine and all of the game logic: about 2.7 MB of ARM and Thumb-2 code, VFPv3,
no NEON. The Java side is a thin wrapper that handles the GL surface, touch input and the OBB.

Since 2022, ARM's big cores (Cortex-X2 and later, Cortex-A715 and later) have dropped AArch32
entirely. Phones built only from such cores (Dimensity 9200 and later, Snapdragon 8 Gen 3 and
later) report only `arm64-v8a`, and so do some phones whose Android build dropped 32-bit (Pixel 7
and later). No setting brings 32-bit back, and without the engine's source code it can't be
recompiled. A decompiler's output is far from something that builds into a working game.

Two things worked in our favour:

- **The library isn't stripped.** It exports 19,581 symbols (`fnaSound_Init`,
  `fnOBBPackages_AddFile` and so on), so it's easy to see what any piece of code does.
- **It imports very little from the system:** 219 functions. These are core OpenGL ES 2.0 (82),
  basic EGL, OpenSL ES, a little libc and pthreads, and one function from libandroid. Nothing
  exotic.

## 2. THUMB

[THUMB](https://github.com/whodfisbane/thumb) runs 32-bit Android apps on 64-bit-only phones. It
works like this:

- Its patcher replaces each `lib/armeabi-v7a/libX.so` with a 64-bit stub, and keeps the original as
  `lib/arm64-v8a/libX_arm32.so`.
- At runtime the stub maps the original into a 4 GB guest address space, and runs it with
  [dynarmic](https://github.com/azahar-emu/dynarmic), an ARM32 → ARM64 JIT.
- System calls go through *thunks*: host implementations of libc, GLES, EGL, liblog and other
  libraries, which take care of the 32-bit/64-bit ABI differences. The game's Java code runs on
  normal 64-bit ART, and a JNI bridge connects the two sides.

What this project relies on is THUMB's **symbol resolution order**:

1. a module's own definition
2. *other loaded guest modules*
3. thunks

Libraries the app ships itself are loaded first as "siblings" (for every `DT_NEEDED` entry, THUMB
looks for `libX_arm32.so` in the app's library folder). So a 32-bit library named `libOpenSLES.so`
or `libGLESv2.so` inside the APK overrides THUMB's version of any function it exports. Every other
function still goes to THUMB. No THUMB changes are needed, and THUMB's on-phone patcher picks the
libraries up like any other `armeabi-v7a` library.

## 3. What was missing: sound

THUMB Doctor reported 211 of 219 imports supported. Missing were seven OpenSL ES symbols
(`slCreateEngine` and six `SL_IID_*` interface IDs) and `AAssetManager_fromJava`.

Calls to unimplemented thunks return 0. In OpenSL ES, 0 is `SL_RESULT_SUCCESS`, so
`fnaSound_Init` believed it had an engine and then called through a null interface pointer:

```
UNIMPLEMENTED slCreateEngine ... called from libLEGO_M1.so+0x2f9c54 (_Z13fnaSound_Initv+0xd4)
guest fault: pc 0x00000000, lr libLEGO_M1.so+0x2f9c6c (_Z13fnaSound_Initv+0xec)
```

Disassembly showed how the game uses OpenSL ES:

- **Sound effects:** a pool of voices, each an `SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE` player with
  PCM. A worker thread restarts a voice by calling `Clear` on its buffer queue, enqueueing 4
  streaming chunks, then setting the play state to playing. Further chunks follow from the
  buffer-queue callback.
- **Music:** `SL_DATALOCATOR_ANDROIDFD` players. A file descriptor plus offset and length point at
  an MP3 inside the OBB, with `SLSeekItf::SetLoop` for looping tracks.

`src/slaudio.c` implements exactly that subset:

- Objects and interface vtables live in guest memory, as real OpenSL ES does. Every interface the
  game touches is implemented (Object, Engine, Play, Volume, AndroidSimpleBufferQueue, Seek,
  PlaybackRate). Anything else is logged and returns `SL_RESULT_FEATURE_UNSUPPORTED`.
- One mixer thread mixes all playing voices:
  - linear resampling to the device rate
  - per-voice gain (millibels), stereo position and playback rate
  - output through a Java `android.media.AudioTrack`, called through THUMB's JNI bridge.
- MP3 players read their whole byte range up front with `pread` (the game may reuse the descriptor)
  and decode with [minimp3](https://github.com/lieff/minimp3).
- Callbacks are collected while mixing and fired after the mixer's lock is released, so the game
  can enqueue from inside a callback without lock-order problems. Destroyed players are freed by the
  mixer between steps.
- **Getting to Java:** the mixer needs a `JavaVM`, and guest code has no `JNI_GetCreatedJavaVMs`.
  But the engine calls `AAssetManager_fromJava(env, ...)` early on, from its
  `nativeInitializeAssetManager`. Exporting that function hands us a guest `JNIEnv`, and
  `GetJavaVM` does the rest. The engine never uses the asset manager (it reads everything from the
  APK and OBB itself), so returning NULL is safe. That's also the eighth missing import.

## 4. The OBB copies going around were broken

The engine doesn't open the OBB through Android. The Java side lists the entries with Google's
`ZipResourceFile`, and passes each entry's name, **data offset and compressed length** to
`fnOBBPackages_AddFileEntry`. The engine then reads raw bytes at those offsets. Two consequences:

1. **Entries must be stored, not compressed.** Read raw, a deflated entry is garbage. The OBB we
   started from had every entry deflated (at 0–2% compression, so it bought nothing), probably
   re-zipped by whoever uploaded it.
2. **Paths must use `/`.** Before a lookup the engine turns `\` into `/` (`fnFile_CopyFilenameFormatted`),
   then compares names with `strcasecmp`. The re-zip had stored `music\eye_of_the_storm.mp3` with
   Windows separators, so no music track was ever found. The `.fib` archives sit at the top level,
   which is why everything else still loaded.

Also: the game looks for `main.<a>.com.lego.ninjago.toe.obb`, where `<a>` comes from the APK's
`assets/obb_info.xml`. The size check there is skipped by a "dev file name" fallback that only
checks the file exists, so an OBB from a slightly different build loads fine if it's named right.

`prepare.py` rewrites the OBB with stored entries and `/` paths, and names it for the APK.

## 5. The black world: per-GPU texture editions

With sound working, the game reached the tutorial, but the 3D world was black apart from a few
highlights. The UI was fine. Logging every `glCompressedTexImage2D` (see `glfix.c`) showed:

```
glCompressedTexImage2D (format 0x8c92) -> GL error 0x500
first upload of texture format 0x87ee
```

`0x8C92` and `0x87EE` are `GL_ATC_RGB_AMD` and `GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD`. ATC is a
texture compression format that only Qualcomm Adreno GPUs implement. Google Play used to deliver
APK and OBB pairs per texture format (`<supports-gl-texture>` in the manifest). The OBB copies going
around are the Adreno edition. Fun fact: the APK we tested with declares DXT1 (Tegra), and another
copy declares ETC1. The engine itself is the same in every edition, and it simply uploads whatever
format the data contains.

On any non-Adreno GPU every ATC upload fails with `GL_INVALID_ENUM`, the textures stay incomplete,
and the prebuilt shaders sample black. The UI uses uncompressed RGBA, so it looks fine. This is
also why the game looked the same in emulators and VMs.

`src/glfix.c` intercepts `glCompressedTexImage2D`:

- If the GPU supports the format natively (ATC on Adreno, S3TC where available), the data goes
  through unchanged.
- Otherwise, each 4×4 block is decoded and the level is uploaded with `glTexImage2D`: RGB565 for
  formats without alpha, RGBA8888 for the rest.
  - **ATC:** endpoint 0 is RGB555 plus a mode bit, endpoint 1 is RGB565, followed by 2-bit
    indices. Layout as in bimg's `decodeBlockATC`. The explicit-alpha and interpolated-alpha
    variants use DXT3-style and DXT5-style alpha blocks.
  - **DXT1/3/5** for the Tegra edition (untested, as we have no data to try it on).

The cost is memory: decoded textures take 4× (RGB565) to 8× (RGBA8888) the GPU memory of the
compressed ones. Loads also take a little longer, as decoding runs as translated 32-bit code.

## 6. Diagnostics on uncooperative phones

On the test phone (vivo, Android 16) game sessions sometimes produced no logcat output at all, not
even THUMB's first line, while other apps logged normally. Both libraries therefore also write
`audio.log` and `graphics.log` into the game's external files folder, which the app can always
write and `adb pull` can always read.

## 7. The optional cheats

TT left their developer cheat switches in the shipped game. `g_CheatOptions` is three exported bytes
that 14 places in the code read:

| Byte | Bit | Read by | Effect |
|---|---|---|---|
| 0 | `0x08` | `SaveGame_IsCharUnlocked`, `IsCharHubUnlocked`, `IsCharBought` | all characters unlocked and bought |
| 0 | `0x10` | `SaveGame_IsCharacterSuperMoveBought`, `…SuperMoveXBought` | all super moves bought |
| 0 | `0x20` | `UIFrontEnd_IsLevelUnlocked` | all levels unlocked |
| 0 | `0x40` | `SaveGame_IsExtraUnlocked`, `IsExtraBought`, `Hud_InitExtrasMenu` | all extras unlocked and bought |
| 1 | `0x04` | `SaveGame_DoCheats` | studs (`SAVEGAMEDATA+0x78`, 64-bit) = 10,000,000 |
| 1 | `0x08` | `SaveGame_DoCheats` | tokens (`SAVEGAMEDATA+0x4a8`) = 10,000,000 |
| 1 | `0x20` | `IsCharUnlocked`, `IsCharHubUnlocked` | all characters unlocked |
| 1 | `0x40` | `IsExtraUnlocked`, red brick screens | all red bricks unlocked |
| 2 | `0x04`/`0x08` | `SaveGame_DoCheats` | one-off +100,000 / +300,000 studs (the old in-app purchases) |

`src/mods.c` looks the symbols up with `dlsym`. That works from a library constructor, because THUMB
runs constructors after the game is loaded and relocated, but before any of its code has run. It
then sets the bits.

`SaveGame_DoCheats` only runs from `SaveGame_InitProfile`, so studs and tokens are also topped up
directly in `SaveGame_Data`, the live save, which the shop reads its "bank balance" from. That
happens from a `glClear` wrapper: `fnaRender_StartRender` calls `glClear` every frame on the game's
own thread, so no other thread touches game state. TT's `SaveGame_CompleteEverything` is called
once, about 10 seconds after start.

## 8. Where to take it next

- **Upstream it.** OpenSL ES support is the top item on THUMB's roadmap, and software texture
  decoding would help other games from the per-GPU-OBB era. Both could live in THUMB as host-side
  thunks, which would be faster than this guest-side version and would help every game.
- **PVRTC decoding**, so the PowerVR edition works too.
- **Re-encoding to ETC2 or ASTC** instead of uncompressed, to get the memory back.
- **Other TT Fusion mobile games** from the same era probably need the same fixes.
