# Drop To Sampler

Расширение для [REAPER](https://www.reaper.fm/) (Windows и macOS): перетаскивание медиа на **TCP / MCP** вставляет сэмплер (RS5k или Cartridge) с загруженным сэмплом вместо вставки айтема на трек.

## Использование

* Перетащите файлы из Media Explorer (или Проводника / Finder) на список треков или микшер.
* В строке статуса под арранжем отображается режим (New sampler / Replace…).

| Действие | Windows | macOS |
|---|---|---|
| один трек / несколько треков | **Shift** | **Shift** |
| Layers / Chromatic | **Ctrl** | **⌘ Cmd** (или Control) |
| Add / Replace (не для режима «on FX slot») | **Alt** | **⌥ Option** |

* Опционально: `sampler_insert.png` / `sampler_replace.png` рядом с плагином (или в `UserPlugins`) подменяют встроенные иконки.

**Настройки:** *Extensions → Drop To Sampler* или *Preferences → Plug-ins → Drop To Sampler*.

## Установка

| Платформа | Файл | Куда |
|---|---|---|
| Windows x64 | `reaper_drop_to_sampler.dll` | `%APPDATA%\REAPER\UserPlugins\` |
| macOS (Apple Silicon + Intel) | `reaper_drop_to_sampler.dylib` | `~/Library/Application Support/REAPER/UserPlugins/` |

Свежие сборки с `main` лежат в ветке [`builds`](../../tree/builds).

На macOS скачанный неподписанный dylib помечается карантином; снять его:

```sh
xattr -d com.apple.quarantine ~/Library/Application\ Support/REAPER/UserPlugins/reaper_drop_to_sampler.dylib
```

## Сборка

Исходники WDL/SWELL подключены как сабмодуль:

```sh
git clone --recursive https://github.com/patch-34/reaper-drop-to-sampler
# или в уже склонированном репо:
git submodule update --init
```

**Windows** — MSYS2 UCRT64: `mingw-w64-ucrt-x86_64-gcc`, `cmake`, `ninja`.

```bat
build.bat
```

`build(extra).bat` — закрывает REAPER, копирует DLL, запускает REAPER снова.

**macOS** — Xcode Command Line Tools и CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# -> build/reaper_drop_to_sampler.dylib (universal: arm64 + x86_64)
```

CI (GitHub Actions) собирает обе платформы на каждый push.

После правки `src/prefs_dialog.rc` перегенерируйте Mac-версию диалога: `tools/gen_mac_resources.sh`.
После замены PNG в `src/res/`: `python3 tools/gen_png_header.py`.

## Как работает

* `src/core.cpp` — вся логика, общая для обеих платформ: сэмплеры, треки, настройки, страница Preferences.
* Нативная вставка айтема блокируется; FX добавляется через `TrackFX_AddByName`.
* RS5k: `FILE0` + `DONE`; Cartridge: `pending_load.txt` + параметр Load Trigger.
* Цель дропа через `GetThingFromPoint` (трек / слот FX).
* Индикация: `Help_Set` + PNG-оверлей у курсора.
* Настройки хранятся в ExtState REAPER.

**Windows** (`src/win/`): OLE `IDropTarget` на окнах TCP/MCP перехватывает дроп; оверлей — layered window (GDI+).

**macOS** (`src/mac/`): OLE нет. Окна REAPER — это `NSView` из SWELL, файлы приходят через Cocoa `NSDraggingDestination`. Плагин подменяет методы `draggingEntered/Updated/Exited` и `performDragOperation` у класса SWELL-вида REAPER и на каждом движении проверяет точку через `GetThingFromPoint`: над TCP/MCP перетаскивание обрабатывает плагин, в остальных местах работает штатная логика REAPER. Оверлей — прозрачная `NSPanel`.

### Cartridge на macOS

Где Cartridge на Mac ждёт `pending_load.txt`, публично не описано. Плагин берёт первую существующую папку из:
`~/Library/Application Support/Cartridge`, `~/Library/Cartridge` (по умолчанию — вторую).
Точный путь можно задать без пересборки, ExtState `DropToSampler / cartridge_dir` (например, из Lua: `reaper.SetExtState("DropToSampler", "cartridge_dir", "/путь", true)`).

---

# Drop To Sampler (English)

REAPER extension for Windows and macOS: drop media on **TCP / MCP** to insert a sampler (RS5k or Cartridge) with the sample loaded, instead of inserting an item on the track.

## Usage

* Drag files from Media Explorer (or Explorer / Finder) onto the track list or mixer.
* The status line under the arrange shows the mode (New sampler / Replace…).

| Action | Windows | macOS |
|---|---|---|
| one track / multiple tracks | **Shift** | **Shift** |
| Layers / Chromatic | **Ctrl** | **⌘ Cmd** (or Control) |
| Add / Replace (not for "on FX slot" mode) | **Alt** | **⌥ Option** |

* Optional: `sampler_insert.png` / `sampler_replace.png` next to the plug-in (or in `UserPlugins`) override the embedded icons.

**Settings:** *Extensions → Drop To Sampler* or *Preferences → Plug-ins → Drop To Sampler*.

## Install

| Platform | File | Location |
|---|---|---|
| Windows x64 | `reaper_drop_to_sampler.dll` | `%APPDATA%\REAPER\UserPlugins\` |
| macOS (Apple Silicon + Intel) | `reaper_drop_to_sampler.dylib` | `~/Library/Application Support/REAPER/UserPlugins/` |

Latest builds from `main` are on the [`builds`](../../tree/builds) branch. On macOS, remove the quarantine flag from a downloaded dylib with `xattr -d com.apple.quarantine <file>`.

## Build

`git submodule update --init` (WDL/SWELL), then:

* **Windows** (MSYS2 UCRT64: gcc, cmake, ninja): `build.bat`
* **macOS** (Xcode CLT + CMake): `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`

## How it works

* `src/core.cpp`: all platform-neutral logic (samplers, tracks, settings, Preferences page).
* **Windows** (`src/win/`): OLE `IDropTarget` on the TCP/MCP windows; layered-window overlay (GDI+).
* **macOS** (`src/mac/`): REAPER's windows are SWELL `NSView`s. The plug-in swizzles the Cocoa dragging-destination methods on REAPER's SWELL view class and hit-tests every drag with `GetThingFromPoint`; over TCP/MCP the drag is handled by the plug-in, elsewhere REAPER's own handling runs unchanged. Overlay is a click-through `NSPanel`.
* Cartridge on macOS: `pending_load.txt` goes to the first existing of `~/Library/Application Support/Cartridge`, `~/Library/Cartridge`; override with ExtState `DropToSampler / cartridge_dir`.
