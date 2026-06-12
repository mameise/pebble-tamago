# tamago-pebble

A port of [asterick/tamago](https://github.com/asterick/tamago) — the
TamaTown Tama-Go (Tamagotchi V7) emulator — to Pebble.

Target: Pebble Time 2 (Emery platform).

## ⚠️ ROM required

This repository ships with a **dummy** `resources/data/tamago.bin` that
boots into a static checkerboard pattern so the project builds and runs
out of the box. The real Tama-Go ROM is copyrighted and not
redistributable — you must dump it yourself or otherwise obtain it
legally, then replace the dummy file at:

```
resources/data/tamago.bin       (must be exactly 655360 bytes / 640 KB)
```

If you see a static striped pattern when you launch the watchface
instead of an egg / room / Tamagotchi character, the dummy ROM is
still loaded.

## Status

Early proof-of-concept. The 6502 CPU core, memory map, ROM banking, and
basic display rendering are in place. Many things are stubbed:

- ⚠ EEPROM emulation not implemented (Tama-Go uses external SPI EEPROM
  for save data; ROM boot-time self-test may fail)
- ⚠ IRQ/NMI timer rates are guessed — may need calibration
- ⚠ Watchface integration is minimal — just a centered scaled framebuffer

See [HOW_TO_BUILD.md](HOW_TO_BUILD.md) for build + debugging notes.

## CloudPebble import

```
https://cloudpebble.repebble.com/ide/import/github/<your-username>/tamago-pebble/
```

After importing, replace `resources/data/tamago.bin` with your own ROM
dump before flashing — otherwise you'll just see the dummy pattern.

## Hardware specs (TamaTown Tama-Go, V7, 2010)

- **CPU**: 8-bit GPLB52320A (6502-family) @ 4 MHz
- **Display**: 64×32 px, 4 grayscale levels
- **ROM**: 640 KB (`tamago.bin`), 20 banks × 32 KB
- **RAM**: 1.5 KB work RAM + 512 B display RAM + 256 B CPU registers
- **EEPROM**: 32 KB external SPI
- **Buttons**: A, B, C, Reset

## Layout

```
tamago-pebble/
├── package.json                       App metadata + resource list
├── wscript                            Pebble build script
├── resources/
│   └── data/
│       └── tamago.bin                 640 KB ROM (committed)
└── src/
    └── c/
        ├── main.c                     Pebble app shell
        └── tamago/
            ├── tamago.h               Public emulator API
            ├── tamago_internal.h      System state struct (private)
            ├── tamago.c               init / step / display / save state
            ├── tamago_cpu.c           6502 core (56 opcodes, 13 modes)
            └── tamago_memory.c        Memory map + ROM banking + I/O regs
```

## Credits

- Original emulator: [@asterick](https://github.com/asterick) — JavaScript
  reverse-engineering and emulator on which this port is based.
- ROM dumps: Natalie Silvanovich (referenced in asterick's repo).

## License

Code: same license as the upstream emulator (see asterick/tamago).
ROM: not redistributable. The `tamago.bin` shipped in this repository
is a dummy file with a minimal boot stub — replace it with your own
dump of the real Tama-Go ROM to use the emulator.
