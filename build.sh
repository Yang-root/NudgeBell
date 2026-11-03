#!/usr/bin/env bash
set -e
cd "$(dirname "$0")"
EXE=NudgeBell.exe
mkdir -p build
taskkill //F //IM NudgeBell.exe >/dev/null 2>&1 || true
taskkill //F //IM VibeFocusLite.exe >/dev/null 2>&1 || true
sleep 0.3
windres --input-format=rc --output-format=coff --include-dir=res res/app.rc -o build/app_res.o
g++ -O2 -std=c++17 -municode -mwindows \
  src/main.cpp src/audio.cpp build/app_res.o \
  -o "$EXE" \
  -lgdiplus -lwinmm -lshell32 -lole32 -luuid -ldwmapi \
  -static -static-libgcc -static-libstdc++ -lwinpthread
ls -la "$EXE"
du -h "$EXE"
