#!/bin/sh
# Builds the launchers from launcher.c, static (no loader, no particular glibc needed):
# AbletonLive12-launcher and AbletonLive11-launcher (LIVE_MAJOR names them).
cd "$(dirname "$0")"
for v in 12 11; do
    gcc -O2 -static -s -DLIVE_MAJOR="\"$v\"" launcher.c -o "AbletonLive$v-launcher" || exit 1
done
