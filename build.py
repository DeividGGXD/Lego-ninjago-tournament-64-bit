#!/usr/bin/env python3
"""Builds the two guest libraries into prebuilt/armeabi-v7a/.

Needs the Android NDK (r25 or newer). It is found through ANDROID_NDK_HOME,
ANDROID_NDK_ROOT or the newest ndk/ folder of the Android SDK
(ANDROID_HOME / ANDROID_SDK_ROOT, or the default SDK location).

    python build.py
"""
import os
import pathlib
import platform
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
SRC = ROOT / "src"
OUT = ROOT / "prebuilt" / "armeabi-v7a"

# THUMB translates ARMv7 code; NEON is avoided on purpose (plain VFP is the
# best-tested path) and so is the stack protector (one less import).
CFLAGS = [
    "--target=armv7a-linux-androideabi21", "-O2", "-fPIC", "-shared", "-fvisibility=hidden",
    "-fno-stack-protector", "-mfpu=vfpv3-d16", "-mfloat-abi=softfp",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-function",
    "-Wl,--no-undefined", "-Wl,-z,max-page-size=4096", "-Wl,--build-id=none",
]

LIBS = [
    ("libOpenSLES.so", ["slaudio.c"], ["-llog", "-lm"]),
    ("libGLESv2.so", ["glfix.c"], ["-lGLESv2", "-lEGL", "-llog"]),
]


def sdk_dirs():
    for var in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(var):
            yield pathlib.Path(os.environ[var])
    home = pathlib.Path.home()
    yield home / "AppData/Local/Android/Sdk"  # Windows
    yield home / "Library/Android/sdk"  # macOS
    yield home / "Android/Sdk"  # Linux


def find_ndk():
    for var in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"):
        if os.environ.get(var):
            return pathlib.Path(os.environ[var])
    for sdk in sdk_dirs():
        ndks = sorted((sdk / "ndk").glob("*")) if (sdk / "ndk").is_dir() else []
        if ndks:
            return ndks[-1]
    sys.exit("Android NDK not found: install it with the SDK manager or set ANDROID_NDK_HOME")


def clang(ndk):
    host = {"Windows": "windows-x86_64", "Darwin": "darwin-x86_64"}.get(platform.system(), "linux-x86_64")
    exe = ndk / "toolchains/llvm/prebuilt" / host / "bin" / ("clang.exe" if host.startswith("windows") else "clang")
    if not exe.exists():
        sys.exit(f"clang not found at {exe}")
    return exe


def main():
    ndk = find_ndk()
    cc = clang(ndk)
    print(f"NDK: {ndk}")
    OUT.mkdir(parents=True, exist_ok=True)
    for name, sources, libs in LIBS:
        out = OUT / name
        cmd = [str(cc), *CFLAGS, f"-Wl,-soname,{name}", "-o", str(out), *[str(SRC / s) for s in sources], *libs]
        print(f"building {name}")
        subprocess.run(cmd, check=True)
        print(f"  {out.relative_to(ROOT)} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
