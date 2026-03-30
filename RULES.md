# DreamDexed — Agentic Coding Rules

> These rules apply to all AI agents working on the DreamDexed codebase.
> DreamDexed is a bare-metal FM synthesizer for Raspberry Pi 3, forked from MiniDexed.

---

## 1. Architecture Overview

DreamDexed runs on **Circle OS** (bare-metal RPi, no Linux). The project produces a single `kernel8.img` loaded by the RPi bootloader. There is **no standard C library runtime** — only Circle's newlib port.

### Key Components
| Component | Path | Description |
|-----------|------|-------------|
| Circle OS | `circle-stdlib/` | Bare-metal OS (submodule, do NOT modify) |
| Synth Engine | `Synth_Dexed/` | Dexed FM synth core (submodule, do NOT modify) |
| CMSIS DSP | `CMSIS_5/` | ARM DSP intrinsics (submodule, do NOT modify) |
| CloudSeed | `CloudSeedCore/` | Reverb DSP (submodule, do NOT modify) |
| Application | `src/` | **All DreamDexed code lives here** |
| u8g2 Library | `src/u8g2/` | Vendored graphics library (minimal subset) |
| Build Output | `sdcard/` | Generated SD card content (gitignored) |

### Submodule Rules
- **NEVER modify submodule contents directly**. Fork and point `.gitmodules` instead.
- The `circle-stdlib` submodule points to an Akashic-Trance-Machines fork with custom drivers (SSD1309, MCP23017).
- Run `git submodule update --init --recursive` after cloning.

---

## 2. Build System

### Toolchain
- **ARM GNU Toolchain** `aarch64-none-elf-gcc` (v15.2+) — NOT the Linux aarch64 toolchain.
- macOS path: `/Applications/ArmGNUToolchain/15.2.rel1/aarch64-none-elf/bin`
- Target: Raspberry Pi 3 (`RPI=3`)

### Build Workflow
```bash
# Set environment
export PATH="/Applications/ArmGNUToolchain/15.2.rel1/aarch64-none-elf/bin:$PATH"
export RPI=3

# Build (always use /tmp staging to avoid path issues)
rm -rf /tmp/DreamDexed_build
rsync -a --delete --exclude=".git" . /tmp/DreamDexed_build/
cd /tmp/DreamDexed_build/src && make clean
cd /tmp/DreamDexed_build && ./local-ci.sh

# Deploy
rsync -a /tmp/DreamDexed_build/sdcard/ ./sdcard/
yes | ./deploy-sdcard.sh
```

### Critical Build Notes
- **Always build in `/tmp/DreamDexed_build/`** — the project path contains spaces which break some tooling.
- **Always `make clean` before building** — stale object files cause false "successful" builds that deploy old cached kernels.
- The `local-ci.sh` script does NOT propagate `make` link errors — **always check the tail output for link failures**.
- `-ffunction-sections -fdata-sections` + `--gc-sections` is active: unused code is stripped. This means adding `.o` files to the Makefile won't increase kernel size unless something references them.
- Compiler flags are set in `circle-stdlib/libs/circle/Rules.mk` (do NOT modify).
- Optimization level is `-O3` (set in `src/Makefile`).

### Adding New Source Files
1. Create `.cpp`/`.c` file in `src/`
2. Add `filename.o` to `OBJS` in `src/Makefile`
3. For C files in subdirectories (e.g., `u8g2/`), ensure the `.c` source file EXISTS — the Makefile pattern rules `%.o: %.c` will find it automatically.
4. Verify the new file actually compiles by checking for its `.o` in the build output.

---

## 3. Coding Conventions

### Formatting (enforced by `.clang-format`)
- **Tabs** for indentation, width 8
- **Allman brace style** (opening brace on new line)
- **No column limit** (`ColumnLimit: 0`)
- Pointers right-aligned: `int *p` not `int* p`
- Constructor initializers after colon, zero continuation indent

### Naming Conventions
| Element | Convention | Example |
|---------|-----------|---------|
| Classes | `C` prefix + PascalCase | `CUserInterface`, `CUI4Row` |
| Member variables | `m_` prefix + type hint | `m_pMiniDexed`, `m_nButtonCount`, `m_bEnabled` |
| Type prefixes | `p` pointer, `n` numeric, `b` bool, `s` string | `m_pConfig`, `m_nWidth`, `m_bUseMCP` |
| Structs | `T` prefix + PascalCase | `TMCPPin`, `TMCPButtonBinding` |
| Static variables | `s_` prefix | `static unsigned s_nLastTick` |
| Constants | `UPPER_SNAKE_CASE` or `static constexpr` | `MAX_MCP_BUTTONS`, `SPI_INACTIVE` |
| Enums (in classes) | PascalCase | `CUIMenu::MenuEventBack` |
| Header guards | `#pragma once` (not `#ifndef`) | — |

### File Naming
- Source: `lowercase.cpp` / `lowercase.c`
- Headers: `lowercase.h`
- Config files: `lowercase.ini` / `lowercase.txt`
- Scripts: `lowercase.sh` / `lowercase.py`
- Subfolders: `lowercase/`

### File Headers
Every source file must have this header:
```cpp
//
// filename.h
//
// DreamDexed - [Brief description]
// Copyright (C) 2024  The DreamDexed Team
//
// [License text or "See 4RowUI.md for the full specification."]
//
```

---

## 4. Hardware Constraints

### Target Platform
- **Raspberry Pi 3** (BCM2837, ARM Cortex-A53, AArch64)
- **1 GB RAM** — kernel loads at `0x80000`
- **No GPU acceleration** — all rendering is CPU-based
- **No floating-point in IRQ context** — audio callbacks must use integer math only

### I/O Devices
| Device | Interface | Driver |
|--------|-----------|--------|
| OLED Display | I2C (SSD1306/SSD1309, 128×64) | `CSSD1306Device` / `CSSD1309Display` |
| I/O Expander | I2C (MCP23017) | `CMCP23017` |
| Audio DAC | I2S / PWM | Circle audio subsystem |
| MIDI | UART / USB | `CSerialMIDIDevice` / `CMIDIKeyboard` |

### Display Rules (128×64 OLED)
- Pixel buffer is 1024 bytes (128×64 ÷ 8)
- u8g2 renders to an internal buffer, which is `memcpy`'d to `CSSD1306Device::GetFrameBuffer()`
- SSD1306 driver has an internal double-buffer with `memcmp` diff-check — clear the buffer before copying to force updates
- **Never call `CTimer` or I2C functions during static initialization** — the timer is not initialized yet

### MCP23017 Pin Naming (in config)
- `GPA0`–`GPA7` = Port A, bits 0–7
- `GPB0`–`GPB7` = Port B, bits 0–7
- `0` = disabled/unused

---

## 5. Configuration System

### `minidexed.ini`
All runtime configuration lives in `src/minidexed.ini`. Properties are loaded via `CPropertiesFatFsFile` (Circle's FAT filesystem property reader).

### Adding a New Config Property
1. Add getter declaration in `src/config.h`
2. Add member variable with proper prefix (`m_n`, `m_b`, `m_s`)
3. Add `Load()` implementation in `src/config.cpp` using `m_Properties.GetString()` or `m_Properties.GetNumber()`
4. Add the property with a sensible default to `src/minidexed.ini`

### Config Property Access Patterns
```cpp
// String property (returns const char*)
const char *CConfig::GetUIMode() const { return m_UIMode.c_str(); }

// Boolean derived from string comparison
bool CConfig::Is4RowUI() const { return m_UIMode == "4row"; }

// Generic property access for dynamic pin parsing
const char *CConfig::GetPropertyString(const char *pName, const char *pDefault) const;
```

---

## 6. UI Architecture

### Two UI Modes
| Mode | Class | Rendering |
|------|-------|-----------|
| `classic` | `CUIMenu` | Text-based via `CCharDevice` (HD44780/SSD1306 text mode) |
| `4row` | `CUI4Row` | Pixel-based via u8g2 → `CSSD1306Device` framebuffer |

### Event Routing (in `CUserInterface`)
When `UIMode=4row`, all input events are routed to `CUI4Row` instead of `CUIMenu`:
- Encoder rotation → `OnEncoderRotate(encoderIndex, direction)`
- Encoder click → `OnEncoderClick(encoderIndex)`
- Navigation buttons → `OnBack()`, `OnScrollUp()`, `OnScrollDown()`
- `DisplayWrite()` returns early to prevent classic text from overwriting the pixel display

### u8g2 Integration
- u8g2 is used in **buffer-only mode** — it never touches I2C directly
- HAL callbacks: `u8x8_byte_circle_noop` (no I2C) + `u8x8_gpio_and_delay_circle` (delays only)
- **Never call `u8g2_InitDisplay()`** — it triggers I2C/timer calls that crash during boot
- Use `U8X8_MSG_DISPLAY_SETUP_MEMORY` + manual buffer setup instead

---

## 7. Error Handling & Debugging

### Assertions
- Use `assert()` for programming errors (null pointers, invalid state)
- Assertions will crash the kernel — use them only for "should never happen" conditions

### Logging
```cpp
#include <circle/logger.h>

// Define log source at file scope
LOGMODULE("mymodule");

// Use logging macros
LOGDBG("Debug message: %u", value);     // stripped in release (not in this build)
LOGNOTE("Info: %s", string);
LOGWARN("Warning: %d", code);
LOGERR("Error: %s", message);
LOGPANIC("Fatal: cannot continue");     // halts the kernel
```

### Common Crash Causes
1. **Static initializer accessing `CTimer`** — timer isn't ready during static init
2. **Missing source files in Makefile** — build appears to succeed but deploys stale kernel
3. **Linker errors hidden by `local-ci.sh`** — always check for `undefined reference` in output
4. **Class size mismatch** — if you add members to a class, ALL translation units must be rebuilt

---

## 8. Testing & Deployment

### Hardware Testing (no emulator)
There is no emulator — all testing requires deploying to the physical RPi 3:

1. Build (`local-ci.sh`)
2. Deploy (`deploy-sdcard.sh`) to the `DREAMDEXED` SD card volume
3. Insert SD card, power on RPi
4. Verify: HDMI output (kernel log) + OLED display

### Bisection Testing Pattern
When debugging boot crashes:
1. Start with known-good `git HEAD`
2. Add changes incrementally
3. Build, deploy, test after EACH increment
4. If crash occurs, the last increment is the cause
5. **Always verify kernel size changed** — if size is identical to previous build, the code wasn't actually included

### Kernel Size Reference
| Build | Approximate Size |
|-------|-----------------|
| Baseline (no 4-row) | ~3.68 MB |
| With u8g2 + ui4row | ~3.75 MB |

---

## 9. Git Workflow

### Commit Messages
Use conventional commits:
```
feat: short description of feature
fix: short description of bug fix
refactor: short description of refactoring
docs: documentation changes
```

### What NOT to Commit
- Build artifacts (`*.o`, `*.d`, `*.elf`, `*.img`, `*.map`)
- `sdcard/` directory
- `.DS_Store`
- Temporary test files (stubs, test harnesses)
- `_agents/` and `.agents/` directories (AI agent config)

---

## 10. Common Pitfalls

| Pitfall | Cause | Solution |
|---------|-------|----------|
| Build succeeds but kernel unchanged | Missing `.c` source for an `.o` in Makefile | Verify all source files exist before building |
| Kernel boots but display blank | `u8g2_InitDisplay()` called too early | Use manual `SETUP_MEMORY` msg instead |
| Classic UI overrides 4-row display | `DisplayWrite()` not guarded | Add `if (m_bUse4RowUI) return;` |
| Buttons/encoders don't work in 4-row | Events routed to `CUIMenu` not `CUI4Row` | Check `m_bUse4RowUI` in all event handlers |
| Build link error not visible | `local-ci.sh` doesn't propagate errors | Check `make` output directly, look for `undefined reference` |
| `--gc-sections` strips your code | Nothing reachable references it | Ensure code is called from a reachable path from `main()` |
