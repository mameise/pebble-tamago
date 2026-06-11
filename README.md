# tamago-pebble

A port of [asterick/tamago](https://github.com/asterick/tamago) — the
TamaTown Tama-Go (Tamagotchi V7) emulator — to Pebble.

Target: Pebble Time 2 (Emery platform).

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
ROM: not redistributable; the included `tamago.bin` is for personal use.
