# Drop To Sampler

Расширение для [REAPER](https://www.reaper.fm/): перетаскивание медиа на **TCP / MCP** вставляет сэмплер (RS5k или Cartridge) с загруженным сэмплом (вместо вставки айтема на трек)

## Использование

* Перетащите файлы из Media Explorer (или Проводника) на список треков или микшер.
* В строке статуса под арранжем отображается режим (New sampler / Replace…).
* **Shift** — один трек / несколько треков
* **Ctrl** — Layers / Chromatic
* **Alt** — Add / Replace (не для режима «on FX slot»)
* Опционально: `sampler_insert.png` / `sampler_replace.png` рядом с DLL подменяют встроенные иконки.

**Настройки:** *Extensions → Drop To Sampler* или *Preferences → Plug-ins → Drop To Sampler*.

## Сборка

Нужен **MSYS2 UCRT64**: `mingw-w64-ucrt-x86\_64-gcc`, `cmake`, `ninja`.

```bat
build.bat
```

Результат: `build\\reaper\_drop\_to\_sampler.dll` → скопировать в `%APPDATA%\\REAPER\\UserPlugins\\`.

`build(extra).bat` — закрывает REAPER, копирует DLL, запускает REAPER снова.

## Как работает

* OLE `IDropTarget` на окнах TCP/MCP перехватывает дроп файлов.
* Нативная вставка айтема блокируется; FX добавляется через `TrackFX\_AddByName`.
* RS5k: `FILE0` + `DONE`; Cartridge: `pending\_load.txt` + параметр Load Trigger.
* Цель дропа через `GetThingFromPoint` (трек / слот FX).
* Индикация: `Help\_Set` + PNG-оверлей (GDI+).
* Настройки хранятся в ExtState REAPER.

\---

# Drop To Sampler (English)

REAPER extension: drop media on **TCP / MCP** to insert a sampler (RS5k or Cartridge) with the sample loaded (instead of inserting an item on track)

## Usage

* Drag files from Media Explorer (or Explorer) onto the track list or mixer.
* The status line under the arrange shows the mode (New sampler / Replace…).
* **Shift** — one track / multiple tracks
* **Ctrl** — Layers / Chromatic
* **Alt** — Add / Replace (not for “on FX slot” mode)
* Optional: `sampler_insert.png` / `sampler_replace.png` next to the DLL override the embedded icons.

**Settings:** *Extensions → Drop To Sampler* or *Preferences → Plug-ins → Drop To Sampler*.

## Build

Requires **MSYS2 UCRT64**: `mingw-w64-ucrt-x86\_64-gcc`, `cmake`, `ninja`.

```bat
build.bat
```

Output: `build\\reaper\_drop\_to\_sampler.dll` → copy to `%APPDATA%\\REAPER\\UserPlugins\\`.

`build(extra).bat` — stops REAPER, copies the DLL, restarts REAPER.

## How it works

* OLE `IDropTarget` on TCP/MCP windows intercepts file drops.
* Native item-insert is suppressed; FX is added via `TrackFX\_AddByName`.
* RS5k: `FILE0` + `DONE`; Cartridge: `pending\_load.txt` + Load Trigger parameter.
* Drop target from `GetThingFromPoint` (track / FX slot).
* Feedback: `Help\_Set` + layered PNG overlay (GDI+).
* Settings stored in REAPER ExtState.

\---

DLL: `reaper\_drop\_to\_sampler.dll` · Windows x64

