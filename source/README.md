# Windrose Stack Patcher

Standalone mod manager for **Windrose** — installs stack size `.pak` mods.

## Features
- Auto-detects game path via Steam registry
- 14 stack size presets (x2–x100, 999–999999)
- Background image, custom borderless window
- Single `.exe`, no external dependencies

## Build

Requirements: **Visual Studio 2022+**, **CMake 3.20+**

1. Place your assets in `assets/` folder (see `resources.rc`)
2. ```
   cmake -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```
3. Output: `build/Release/WindroseStackPatcher.exe`

## Assets needed (`assets/` folder)
- `background.png` — background image
- `Stack_Size_Changes_x02_P.pak` … `Stack_Size_Changes_999999_P.pak` — 14 pak files
- `app.ico` — application icon (in root, not assets/)
