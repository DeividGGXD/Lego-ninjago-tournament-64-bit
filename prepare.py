#!/usr/bin/env python3
"""Prepares your own copy of LEGO Ninjago: Tournament for THUMB.

    python prepare.py --apk game.apk --obb main.XXXX.com.lego.ninjago.toe.obb

Writes to ./output (change with --out):
  NinjagoTournament-THUMB-input.apk    the game APK with the audio and graphics
                                       libraries added; patch it with THUMB
  main.<version>.com.lego.ninjago.toe.obb
                                       the game data, fixed and named for the APK

Either --apk or --obb can be given on its own. Add --mods for the optional
cheats (infinite studs and tokens, everything unlocked; see README.md). Only
the Python standard library is needed (Python 3.8+).
"""
import argparse
import pathlib
import re
import shutil
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent
PREBUILT = ROOT / "prebuilt" / "armeabi-v7a"
PREBUILT_MODS = ROOT / "prebuilt" / "armeabi-v7a-mods"  # replaces libraries of the same name
GUEST_LIBS = ["libOpenSLES.so", "libGLESv2.so"]
PACKAGE = "com.lego.ninjago.toe"
GAME_LIB = "lib/armeabi-v7a/libLEGO_M1.so"
REQUIRED_DATA = ["lego_m1_android.fib", "prebuiltshaders_android.fib"]


def fail(msg):
    sys.exit(f"error: {msg}")


def obb_version_of(apk):
    """The OBB version the game looks for, from assets/obb_info.xml."""
    try:
        xml = apk.read("assets/obb_info.xml").decode("utf-8", "replace")
    except KeyError:
        return None
    m = re.search(r'<a\s+value\s*=\s*"(\d+)"', xml)
    return int(m.group(1)) if m else None


def prepare_apk(src, out_dir, mods):
    with zipfile.ZipFile(src) as apk:
        names = set(apk.namelist())
        if GAME_LIB not in names:
            fail(f"{src.name} has no {GAME_LIB}: this isn't the LEGO Ninjago: Tournament APK")
        if "lib/arm64-v8a/libthumb.so" in names:
            fail(f"{src.name} was already patched by THUMB: start from the original APK")
        version = obb_version_of(apk)
        out = out_dir / ("NinjagoTournament-THUMB-input-mods.apk" if mods else "NinjagoTournament-THUMB-input.apk")
        with zipfile.ZipFile(out, "w") as dst:
            for info in apk.infolist():
                if info.filename in (f"lib/armeabi-v7a/{lib}" for lib in GUEST_LIBS):
                    continue  # an older copy of ours
                dst.writestr(info, apk.read(info.filename), compress_type=info.compress_type)
            for lib in GUEST_LIBS:
                path = PREBUILT_MODS / lib if mods and (PREBUILT_MODS / lib).exists() else PREBUILT / lib
                if not path.exists():
                    fail(f"{path} is missing: run build.py or download the release")
                dst.write(path, f"lib/armeabi-v7a/{lib}", compress_type=zipfile.ZIP_DEFLATED)
    print(f"APK: {out}" + (" (with mods)" if mods else ""))
    print(f"     looks for OBB version {version}" if version else "     (no obb_info.xml found)")
    return version


def prepare_obb(src, out_dir, version):
    if version is None:
        m = re.match(r"main\.(\d+)\.", src.name)
        if not m:
            fail("can't tell the OBB version: pass --apk too, or --version")
        version = int(m.group(1))
    out = out_dir / f"main.{version}.{PACKAGE}.obb"
    if out.resolve() == src.resolve():
        fail("the output would overwrite the input: use a different --out folder")
    with zipfile.ZipFile(src) as obb:
        infos = obb.infolist()
        names = {i.filename.replace("\\", "/").lower() for i in infos}
        missing = [n for n in REQUIRED_DATA if n not in names]
        if missing:
            fail(f"{src.name} doesn't look like the game data (missing {', '.join(missing)})")
        compressed = sum(1 for i in infos if i.compress_type != zipfile.ZIP_STORED)
        # orig_filename: on Windows, zipfile already turns '\' into '/' in filename.
        backslashes = sum(1 for i in infos if "\\" in i.orig_filename)
        if compressed:
            print(f"OBB: {compressed} compressed entries (the game can only read stored ones): fixing")
        if backslashes:
            print(f"OBB: {backslashes} entries use '\\' in their path (music won't be found): fixing")
        with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as dst:
            for info in infos:
                name = info.filename.replace("\\", "/")
                if name.endswith(".backup") or name.endswith("/"):
                    continue
                new = zipfile.ZipInfo(name, date_time=info.date_time)
                new.compress_type = zipfile.ZIP_STORED
                new.external_attr = info.external_attr
                with obb.open(info) as r, dst.open(new, "w") as w:
                    shutil.copyfileobj(r, w, 1 << 20)
    print(f"OBB: {out} ({out.stat().st_size // (1024 * 1024)} MB)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apk", type=pathlib.Path, help="the original game APK")
    ap.add_argument("--obb", type=pathlib.Path, help="the game's OBB data file")
    ap.add_argument("--version", type=int, help="OBB version to name the output for (default: from the APK)")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("output"), help="output folder")
    ap.add_argument("--mods", action="store_true", help="include the optional cheats")
    args = ap.parse_args()
    if not args.apk and not args.obb:
        ap.error("give --apk, --obb or both")
    for p in (args.apk, args.obb):
        if p and not p.is_file():
            fail(f"{p} not found")
    args.out.mkdir(parents=True, exist_ok=True)

    version = args.version
    if args.apk:
        apk_version = prepare_apk(args.apk, args.out, args.mods)
        version = version or apk_version
    if args.obb:
        prepare_obb(args.obb, args.out, version)

    print("\nNext: install THUMB on the phone, patch the APK with it, then copy the OBB to")
    print(f"      Android/obb/{PACKAGE}/ (see README.md)")


if __name__ == "__main__":
    main()
