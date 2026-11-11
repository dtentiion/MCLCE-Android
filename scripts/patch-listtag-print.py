#!/usr/bin/env python3
"""ListTag::print compiles with the NDK's clang.

ListTag<T>::print(char *, ostream) hands its ostream to Tag::print, which
takes a wostream. Nothing instantiates it, and MSVC and Xcode's clang let
it by, but the NDK's clang checks the call when it parses the template
because nothing in it depends on T. Going through T makes it dependent, so
it's only checked if something ever uses it.

Idempotent.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "ios" / "upstream" / "Minecraft.World" / "ListTag.h"

OLD = "\t\t\tit->print(newPrefix, out);"
NEW = "\t\t\tstatic_cast<T *>(it)->print(newPrefix, out);"

if not SRC.exists():
    sys.exit(f"missing: {SRC}")
src = SRC.read_bytes().decode("utf-8", errors="replace")
if NEW in src:
    print(f"already patched: {SRC}")
    sys.exit(0)
if src.count(OLD) != 1:
    sys.exit(f"anchor not found once in {SRC}")
SRC.write_text(src.replace(OLD, NEW), encoding="utf-8", errors="replace", newline="")
print(f"patched: {SRC}")
