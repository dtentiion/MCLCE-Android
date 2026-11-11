#!/usr/bin/env bash
# The upstream source as this port builds it: the iOS port's patches first,
# then the ones only the NDK needs. Run once after the submodules are in.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

bash "$REPO_ROOT/ios/scripts/patch-upstream-stdafx.sh"
python3 "$REPO_ROOT/scripts/patch-listtag-print.py"
python3 "$REPO_ROOT/scripts/patch-device-name.py"
python3 "$REPO_ROOT/scripts/patch-chunks-near-first.py"
python3 "$REPO_ROOT/scripts/patch-crash-handler.py"
