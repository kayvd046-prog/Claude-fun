#!/bin/sh
# Cross-compileert SchermPresets.exe voor Windows (x64) met mingw-w64.
# Op Debian/Ubuntu: sudo apt install mingw-w64
set -e
cd "$(dirname "$0")"

x86_64-w64-mingw32-windres src/app.rc -O coff -o app.res
x86_64-w64-mingw32-gcc src/main.c app.res -o dist/SchermPresets.exe \
    -municode -mwindows -O2 -s -static \
    -Wall -Wextra \
    -luser32 -lgdi32 -lshell32
rm -f app.res

echo "Klaar: dist/SchermPresets.exe"
