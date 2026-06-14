# TamaGo Pebble

A TamaTown Tama-Go (Tamagotchi V7, 2010) emulator running as a Pebble app.
Cycle-accurate 6502 core, full 4-shade greyscale LCD, EEPROM persistence,
attention-vibration, RTC sync, and a customizable watch-face shell around
the emulated screen.

![Tama-Go on Pebble](docs/screenshot.png) <!-- add a real screenshot here -->

---

## Features

**Emulator**
- Full 6502-family CPU (GPLB52320A) at ~4 MHz wall-clock throughput
- 640 KB banked ROM (20 × 32 KB banks) + 1.5 KB WRAM + 512 B DRAM
- 48 × 31 px, 4-shade greyscale LCD scaled 2× on the watch
- 10-icon status row (Information, Food, Toilet, Doors, Training,
  IR Comm, Medicine, Gotchi Figure, Album, Attention)
- 4 KB I²C EEPROM emulated → persisted to Pebble storage between launches
- IRQ + NMI scheduling matches the real 60 Hz LCD frame rate
- Time-sync: Tama's internal clock is kept within ±2 s of the Pebble RTC

**Watch-face shell**
- Analog hour/minute hands (configurable color, outline, thickness)
- Digital time + date + battery percentage (configurable color + outline)
- 12 hour markers — Arabic numerals, Roman numerals, or ticks
- Dynamic Tama "device frame" that expands/contracts with active icons
- Vibration on attention (bell icon) — rising-edge detection, 5 s cooldown
- 13 user-configurable settings via the Clay config UI
- Reset toggle to wipe the Tama and start with a fresh egg

**Controls**
- **Up** → button A (left on real Tama-Go)
- **Select** → button B (middle / Execute)
- **Back** → button C (right / Cancel)
- Hold the same buttons to repeat presses

---

## Hardware compatibility

Only **Pebble Time 2 (Emery)** is supported. The emulator needs the
200 × 228 colour display and the headroom of the Emery CPU; older Pebble
hardware cannot keep up with 4 MHz 6502 emulation.

---

## ⚠️ You need to supply your own ROM

The repository ships with a **dummy ROM** (`resources/data/tamago.bin`)
that is not a real Tama-Go ROM and won't run the Tamagotchi firmware.

To use this app you must replace it with a dump of the genuine
TamaTown Tama-Go ROM:

- **Expected size:** exactly 655 360 bytes (640 KB)
- **Expected MD5:** `3efc1835975c964a74b5ef8528bd177b`
- **Expected layout:** 20 contiguous 32 KB banks; bank 0 first 16 KB
  becomes the static $C000–$FFFF region

Where to get the ROM is your own responsibility — dump it from a real
device you own. This project does not provide or link to ROM images
because the Tama-Go firmware is copyrighted by Bandai.

Replace `resources/data/tamago.bin` with your dump before building.

---

## Quick install (recommended: CloudPebble)

1. **Get a Tama-Go ROM** (see above).
2. **Fork or download this repo.**
3. **Replace `resources/data/tamago.bin`** with your ROM dump.
4. Open <https://cloudpebble.net/> and sign in with your Rebble account.
5. **Create new project** → **Import from ZIP** and upload a ZIP of this
   repository (with your ROM in place).
6. In project settings, confirm:
   - Type: Pebble app
   - Target platforms: Emery only
   - SDK 3
7. Hit **Compile** → install on a connected Pebble Time 2 via the
   "Phone" tab (or download the `.pbw` and side-load).

> **Note**: CloudPebble expects the ROM in the resource named `TAMA_ROM`.
> The repo's `package.json` already wires it up — no manual setup needed
> as long as the file is at `resources/data/tamago.bin`.

---

## Building locally

If you prefer a local SDK build:

```sh
# Pebble SDK 4.5 / arm-none-eabi-gcc required
npm install                  # pulls @rebble/clay
pebble build                 # produces build/emery/pebble-app.elf + .pbw
pebble install --emulator emery   # for the emulator
pebble install --phone <IP>       # for a real watch
```

---

## Settings (Clay config)

Open the app's settings in the Pebble phone app to configure:

| Section | Setting | Default |
|---|---|---|
| Notifications | Vibration on attention | On |
| Notifications | Sound | Off |
| Notifications | Sound volume | 60 (0–100) |
| Tama display | Show frame | On |
| Tama display | Frame color | White |
| Tama display | Tama pixel color | Black (4-shade ramp blends to white) |
| Watch face | Background fill | Light grey |
| Watch face | Hour-marker color | Black |
| Watch face | Hour-marker style | Arabic / Roman / Ticks |
| Hands | Hands color | Black |
| Hands | Hands outline color | White |
| Hands | Hands thickness | Thick / Medium / Thin |
| Text | Text color | Black |
| Text | Text outline | Off |
| Text | Text outline color | White |
| Time & Date | Time format | System default / 24h / 12h |
| Time & Date | Date format | European (Fri 12 Jun) / American (Fri Jun 12) / ISO (2026-06-12) |
| Reset | Reset Tamagotchi | (toggle, see below) |

The **Tama pixel color** setting derives all four LCD greyscale shades
from one user-chosen color by blending toward white — pick any color
and the LCD will look natural in that hue.

---

## Sound

Pebble Time 2 has a piezo speaker, exposed by the Rebble SDK via
`speaker_play_tone(freq, duration, volume, waveform)`. The Tama-Go
SPU registers ($3050–$306F) are detected by the emulator core on
write and a short square-wave tone is played for each event.

- **Sound** toggle in Notifications → Sound (off by default).
- **Sound volume** slider (0–100, default 60).
- Vibration (Notifications → Vibration on attention) is independent
  and only fires on the bell-icon rising edge — it's unaffected by
  the Sound setting.

The frequency mapping uses `freq = 960 000 ÷ period`, calibrated so
the typical menu-click period (~640) lands near 1500 Hz, matching
the original device pitch.

Detection is platform-independent (in `tamago_memory.c`). On hardware
without `PBL_SPEAKER`, `check_sound_event()` runs but the speaker
calls are `#if`'d out — no-op, no warning. Ports to other platforms
with real audio replace just the `speaker_play_tone`/`speaker_stop`
calls.

---

## Resetting your Tama

To start over with a fresh egg:

1. Open settings in the Pebble phone app
2. Scroll to **Reset** → toggle **Reset Tamagotchi** to ON
3. Hit **Save Settings**
4. The Tama EEPROM is wiped and the CPU resets. Next time you open the
   app you'll see the egg-select screen.

The toggle automatically returns to OFF after the reset takes effect.

---

## Project structure

```
.
├── package.json                # Pebble app manifest + message keys
├── wscript                     # waf build script
├── resources/
│   └── data/
│       └── tamago.bin          # ← REPLACE WITH YOUR ROM
├── src/
│   ├── pkjs/
│   │   ├── index.js            # Clay bridge (minimal)
│   │   └── config.js           # Clay UI schema
│   └── c/
│       ├── main.c              # Watch-face shell, settings, timers,
│       │                       # input handling, dirty tracking
│       └── tamago/
│           ├── tamago.h        # Public emulator API
│           ├── tamago.c        # Top-level integration
│           ├── tamago_internal.h
│           ├── tamago_cpu.c    # 6502 core (lazy NZ, -O2)
│           ├── tamago_memory.c # Page tables, I/O, ROM banking
│           ├── tamago_eeprom.c # I²C EEPROM emulation
│           ├── tamago_eeprom.h
│           ├── tamago_rtc_sync.c   # Tama-clock ↔ Pebble RTC
│           └── tamago_rtc_sync.h
```

---

## Performance notes

On Emery (Pebble Time 2) this runs at ~98% of the original Tama-Go
4 MHz clock with the default settings. The pacing loop is deadline-
based with a 3 ms guard for OS responsiveness (clicks, backlight,
BT). With the RTC sync running every 30 s, observable clock drift
stays under ~2 seconds.

Battery life is reduced compared to a static watchface because the
emulator runs continuously. Expect roughly 1–2 days between charges
versus 5–7 days for a regular watchface.

If you want to dig into the numbers, flip `TAMAGO_PROFILE` to `1` in
`src/c/tamago/tamago.h`, rebuild, and watch the per-minute log line:

```
prof: ops=… r=…(io=…) w=…(io=… drop=…)
prof: irqs=…(entered=…) nmis=…(entered=…)
```

Turn it back off for production — the counters cost ~10–15 % CPU.

---

## Credits

- 6502 core + ROM-banking logic ported from
  [asterick/tamago](https://github.com/asterick/tamago) (JavaScript
  reference emulator).
- Hardware documentation from
  [TheCuttingRoomFloor wiki](https://tcrf.net/TamaTown_Tama-Go),
  [Tamagotchi Fandom wiki](https://tamagotchi.fandom.com/wiki/TamaTown_Tama-Go),
  and the Bandai Quick Start Guide.
- Clay settings framework from
  [@pebble-dev/clay](https://github.com/pebble-dev/clay).

TamaTown Tama-Go, Tamagotchi and all related names/logos are
trademarks of Bandai. This project is unaffiliated.

---

## License

Source code in this repository (everything except `resources/data/tamago.bin`)
is released under the MIT license — see `LICENSE`. The ROM is property of
Bandai and must be supplied by the user.
