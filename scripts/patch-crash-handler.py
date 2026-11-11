#!/usr/bin/env python3
"""The game's crash handler hands the signal on to the system's.

The iOS tree's handler writes a line to its crash log, puts the default
action back and raises the signal again. Here the handler that was there
before it is the system's, the one that writes the tombstone with the
backtrace, so that one goes back on instead. The app keeps them in
mcle_android_restore_signal.

Idempotent.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "ios" / "Minecraft.Client" / "iOS" / "Game" / "MCLEGameLoop.cpp"

MARK = "mcle_android_restore_signal"

OLD_HANDLER = "static void mcle_crash_handler(int sig, siginfo_t *info, void *) {\n"
NEW_HANDLER = (
    "#ifdef __ANDROID__\n"
    "extern \"C\" void mcle_android_restore_signal(int sig);\n"
    "#endif\n"
    "\n"
    "static void mcle_crash_handler(int sig, siginfo_t *info, void *) {\n"
)

OLD_RAISE = (
    "    struct sigaction dfl{};\n"
    "    dfl.sa_handler = SIG_DFL;\n"
    "    sigemptyset(&dfl.sa_mask);\n"
    "    sigaction(sig, &dfl, nullptr);\n"
    "    raise(sig);\n"
)
NEW_RAISE = (
    "#ifdef __ANDROID__\n"
    "    mcle_android_restore_signal(sig);\n"
    "#else\n"
    "    struct sigaction dfl{};\n"
    "    dfl.sa_handler = SIG_DFL;\n"
    "    sigemptyset(&dfl.sa_mask);\n"
    "    sigaction(sig, &dfl, nullptr);\n"
    "#endif\n"
    "    raise(sig);\n"
)

if not SRC.exists():
    sys.exit(f"missing: {SRC}")
src = SRC.read_bytes().decode("utf-8")
nl = "\r\n" if "\r\n" in src else "\n"
if MARK in src:
    print(f"already patched: {SRC}")
    sys.exit(0)
for old, new in ((OLD_HANDLER, NEW_HANDLER), (OLD_RAISE, NEW_RAISE)):
    old = old.replace("\n", nl)
    if src.count(old) != 1:
        sys.exit(f"anchor not found once in {SRC}: {old.splitlines()[0].strip()}")
    src = src.replace(old, new.replace("\n", nl))
SRC.write_bytes(src.encode("utf-8"))
print(f"patched: {SRC}")
