#!/usr/bin/env python3
"""The strings that name the console name this device.

The iOS port swaps "Xbox One console" for the device's name, which it
works out from uname's machine id and a table of Apple's ids. Here the
name is the system's ro.product.model ("Pixel 3a"), read at the top of
device_name() in the iOS tree's Game/MCLEStrings.cpp.

Idempotent.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "ios" / "Minecraft.Client" / "iOS" / "Game" / "MCLEStrings.cpp"

OLD = (
    "std::wstring device_name() {\n"
    "    struct utsname u;\n"
)
NEW = (
    "std::wstring device_name() {\n"
    "#ifdef __ANDROID__\n"
    "    char model[PROP_VALUE_MAX] = {};\n"
    "    __system_property_get(\"ro.product.model\", model);\n"
    "    if (model[0]) {\n"
    "        const std::string name = model;\n"
    "        return std::wstring(name.begin(), name.end());\n"
    "    }\n"
    "#endif\n"
    "    struct utsname u;\n"
)
INCLUDE_OLD = "#include <sys/utsname.h>\n"
INCLUDE_NEW = "#include <sys/utsname.h>\n#ifdef __ANDROID__\n#include <sys/system_properties.h>\n#endif\n"

if not SRC.exists():
    sys.exit(f"missing: {SRC}")
src = SRC.read_bytes().decode("utf-8")
nl = "\r\n" if "\r\n" in src else "\n"
if "ro.product.model" in src:
    print(f"already patched: {SRC}")
    sys.exit(0)
for old, new in ((OLD, NEW), (INCLUDE_OLD, INCLUDE_NEW)):
    old = old.replace("\n", nl)
    if src.count(old) != 1:
        sys.exit(f"anchor not found once in {SRC}: {old.splitlines()[0]}")
    src = src.replace(old, new.replace("\n", nl))
SRC.write_bytes(src.encode("utf-8"))
print(f"patched: {SRC}")
