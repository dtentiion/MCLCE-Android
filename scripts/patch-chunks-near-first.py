#!/usr/bin/env python3
"""LevelRenderer::renderChunks draws the nearest chunks first.

The consoles draw the chunks in the order they sit in the array. Phone GPUs
only skip a hidden pixel if what hides it was drawn before, so here they go
out nearest first. What ends up on screen is the same.

Idempotent.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "ios" / "upstream" / "Minecraft.Client" / "LevelRenderer.cpp"

MARK = "nearFirst"

OLD_LOOP = (
    "\tbool first = true;\n"
    "\tint count = 0;\n"
    "\tClipChunk *pClipChunk = chunks[playerIndex].data;\n"
)
NEW_LOOP = (
    "\tbool first = true;\n"
    "\tint count = 0;\n"
    "#ifdef __ANDROID__\n"
    "\tstatic vector< pair<float, int> > nearFirst;\n"
    "\tnearFirst.clear();\n"
    "#endif\n"
    "\tClipChunk *pClipChunk = chunks[playerIndex].data;\n"
)

OLD_CALL = (
    "\t\tif(RenderManager.CBuffCall(list, first))\n"
    "\t\t{\n"
    "\t\t\tfirst = false;\n"
    "\t\t}\n"
    "\t\tcount++;\n"
    "\t}\n"
)
NEW_CALL = (
    "#ifdef __ANDROID__\n"
    "\t\tfloat xd = pClipChunk->xm - static_cast<float>(xOff);\n"
    "\t\tfloat yd = pClipChunk->ym - static_cast<float>(yOff);\n"
    "\t\tfloat zd = pClipChunk->zm - static_cast<float>(zOff);\n"
    "\t\tnearFirst.push_back(make_pair(xd * xd + yd * yd + zd * zd, list));\n"
    "#else\n"
    "\t\tif(RenderManager.CBuffCall(list, first))\n"
    "\t\t{\n"
    "\t\t\tfirst = false;\n"
    "\t\t}\n"
    "#endif\n"
    "\t\tcount++;\n"
    "\t}\n"
    "#ifdef __ANDROID__\n"
    "\tsort(nearFirst.begin(), nearFirst.end());\n"
    "\tfor( size_t i = 0; i < nearFirst.size(); i++ )\n"
    "\t{\n"
    "\t\tif(RenderManager.CBuffCall(nearFirst[i].second, first))\n"
    "\t\t{\n"
    "\t\t\tfirst = false;\n"
    "\t\t}\n"
    "\t}\n"
    "#endif\n"
)

if not SRC.exists():
    sys.exit(f"missing: {SRC}")
raw = SRC.read_bytes().decode("utf-8", errors="replace")
if MARK in raw:
    print(f"already patched: {SRC}")
    sys.exit(0)
crlf = "\r\n" in raw
src = raw.replace("\r\n", "\n")
for old in (OLD_LOOP, OLD_CALL):
    if src.count(old) != 1:
        sys.exit(f"anchor not found once in {SRC}: {old.splitlines()[0].strip()}")
src = src.replace(OLD_LOOP, NEW_LOOP).replace(OLD_CALL, NEW_CALL)
if crlf:
    src = src.replace("\n", "\r\n")
SRC.write_text(src, encoding="utf-8", errors="replace", newline="")
print(f"patched: {SRC}")
