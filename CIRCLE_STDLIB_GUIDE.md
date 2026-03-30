# Circle-stdlib Integration Guide

> **Purpose**: How the DreamDexed project uses Circle OS and circle-stdlib to run
> bare-metal on Raspberry Pi. Use this as a template for porting other synth
> projects to this hardware platform.

---

## 1. What is Circle / circle-stdlib?

**Circle** is a C++ bare-metal environment for Raspberry Pi. It replaces Linux
entirely — no kernel, no OS, no processes. Your code IS the kernel.

**circle-stdlib** wraps Circle with a newlib C library port, giving you standard
C/C++ functions (`printf`, `malloc`, `std::string`, `<cstdint>`, etc.) while
still running bare-metal.

### Why Bare-Metal?
- **Deterministic latency**: No scheduler, no context switches, no kernel preemption
- **Direct hardware access**: I2C, SPI, GPIO, I2S without drivers/permissions
- **Boot in < 2 seconds**: From power-on to audio output
- **Ideal for real-time audio**: No jitter from OS interrupts

### What You Get
| Feature | Provided by |
|---------|------------|
| C++ runtime, `new`/`delete` | circle-stdlib (newlib) |
| `std::string`, `<cstdint>`, `<cstring>` | circle-stdlib (newlib) |
| `printf`, `snprintf`, `malloc` | circle-stdlib (newlib) |
| I2C, SPI, GPIO, UART, USB | Circle core |
| FAT filesystem (SD card) | Circle addon (FatFs) |
| I2S / PWM audio | Circle sound library |
| HDMI console output | Circle screen device |
| Timer, interrupts, multi-core | Circle core |
| Network (TCP/IP, WLAN) | Circle net addon |
| Display drivers (SSD1306, HD44780, ST7789) | Circle display addon |
| MCP23017 I/O expander | Circle gpio addon |

### What You DON'T Get
- No `<iostream>` (no cin/cout — use `snprintf` + Circle's `CLogger`)
- No `<thread>`, `<mutex>` (use Circle's multi-core API instead)
- No `<filesystem>` (use FatFs `f_open`/`f_read` directly)
- No exceptions or RTTI (disabled via `-fno-exceptions -fno-rtti`)
- No dynamic linking (everything is statically linked into one `kernel8.img`)

---

## 2. Project Structure

```
MyProject/
├── circle-stdlib/               ← git submodule (DO NOT MODIFY)
│   ├── configure                ← generates Config.mk for your target
│   ├── libs/
│   │   └── circle/              ← Circle OS core (nested submodule)
│   │       ├── addon/
│   │       │   ├── display/     ← SSD1306, SSD1309, HD44780, ST7789
│   │       │   ├── gpio/        ← MCP23017 I/O expander
│   │       │   ├── sensor/      ← temperature sensors
│   │       │   ├── Properties/  ← INI file parser
│   │       │   ├── SDCard/      ← SD card driver (EMMC)
│   │       │   └── fatfs/       ← FAT filesystem (FatFs)
│   │       ├── boot/            ← RPi boot files (start.elf, etc.)
│   │       ├── include/circle/  ← Circle API headers
│   │       ├── lib/             ← Core libs (usb, sound, input, net, fs)
│   │       ├── circle.ld        ← Linker script
│   │       └── Rules.mk         ← Compiler/linker flags & build rules
│   └── install/                 ← Compiled newlib (populated by `make`)
│
├── src/                         ← YOUR APPLICATION CODE
│   ├── main.cpp                 ← Entry point
│   ├── kernel.cpp/.h            ← CKernel class (inherits CStdlibAppStdio)
│   ├── circle_stdlib_app.h      ← Base app class with stdio/filesystem
│   ├── config.cpp/.h            ← INI file configuration
│   ├── Makefile                 ← Your OBJS + includes Rules.mk
│   ├── Rules.mk                 ← Links Circle & newlib libraries
│   └── Synth_Dexed.mk          ← Additional synth engine objects (optional)
│
├── build.sh                     ← Configure + compile everything
├── local-ci.sh                  ← Build + create SD card layout
├── deploy-sdcard.sh             ← Copy to mounted SD card
└── sdcard/                      ← Generated output (boot files + kernel)
```

---

## 3. The Build Pipeline

### Step 1: Configure circle-stdlib

```bash
export RPI=3
export TOOLCHAIN_PREFIX="aarch64-none-elf-"

cd circle-stdlib/
./configure -r ${RPI} --prefix "${TOOLCHAIN_PREFIX}" \
    -o SAVE_VFP_REGS_ON_IRQ \
    -o REALTIME \
    -o ARM_ALLOW_MULTI_CORE \
    -o KERNEL_MAX_SIZE=0x400000
make -j
```

**What `configure` does:**
- Generates `Config.mk` with `RASPPI=3`, `AARCH=64`, toolchain prefix
- Passes `-o` flags as `-DFLAG` defines to the compiler
- Builds newlib C library for the target architecture

**Key `-o` options:**
| Option | Purpose |
|--------|---------|
| `SAVE_VFP_REGS_ON_IRQ` | Save floating-point regs in IRQ context (required for audio) |
| `REALTIME` | Enable real-time scheduling optimizations |
| `ARM_ALLOW_MULTI_CORE` | Enable multi-core support (RPi2+) |
| `KERNEL_MAX_SIZE=0x400000` | Set maximum kernel size (4MB) |
| `USE_SDHOST` | Use SD host controller (frees USB for RPi3) |

### Step 2: Build Circle addon libraries

```bash
cd circle-stdlib/libs/circle/addon/display/
make -j       # SSD1306, SSD1309, ST7789 display drivers

cd ../gpio/
make -j       # MCP23017 I/O expander driver

cd ../sensor/
make -j       # Temperature sensor (for status display)

cd ../Properties/
make -j       # INI file parser (for config files)
```

Each addon produces a `.a` static library (e.g., `libdisplay.a`, `libgpio.a`).

### Step 3: Build your application

```bash
cd src/
make -j       # Compiles your code, links everything → kernel8.img
```

### Build Output

The final `kernel8.img` is a flat binary loaded at address `0x80000` by the
RPi bootloader. This IS your entire operating system + application.

| RPi | Target file | Load address | Architecture |
|-----|------------|:------------:|:------------:|
| 1   | `kernel.img` | `0x8000` | ARM 32-bit |
| 2   | `kernel7.img` | `0x8000` | ARM 32-bit |
| 3   | `kernel8.img` | `0x80000` | AArch64 |
| 4   | `kernel8-rpi4.img` | `0x80000` | AArch64 |
| 5   | `kernel_2712.img` | `0x80000` | AArch64 |

---

## 4. Application Architecture

### Inheritance Chain
```
CStdlibApp              ← Interrupt system, GPIO, LED
  └─ CStdlibAppScreen   ← HDMI screen, timer, logger
       └─ CStdlibAppStdio ← SD card, FAT filesystem, USB, console
            └─ CKernel    ← YOUR class (add I2C, SPI, synth engine)
```

### Lifecycle

```
main()
  └─ CKernel kernel;
     └─ kernel.Initialize()
        ├─ CStdlibAppStdio::Initialize()  ← interrupts, screen, timer,
        │                                    logger, SD card, USB, FatFs
        ├─ m_GPIOManager.Initialize()      ← GPIO interrupt handling
        ├─ m_I2CMaster.Initialize()        ← I2C bus for OLED + MCP23017
        ├─ m_Config.Load()                 ← read minidexed.ini from SD
        └─ m_pSynth->Initialize()          ← your synth engine
     └─ kernel.Run()
        └─ while (true)                    ← main loop, runs forever
           ├─ m_pUSB->UpdatePlugAndPlay()  ← USB device hot-plug
           ├─ m_pSynth->Process()          ← audio + UI update
           └─ m_CPUThrottle.Update()       ← thermal management
```

### Entry Point (main.cpp)

```cpp
#include <circle/startup.h>
#include "circle_stdlib_app.h"
#include "kernel.h"

int main()
{
    CKernel Kernel;
    if (!Kernel.Initialize())
    {
        halt();
        return EXIT_HALT;
    }

    CStdlibApp::TShutdownMode ShutdownMode = Kernel.Run();
    Kernel.Cleanup();

    switch (ShutdownMode)
    {
    case CStdlibApp::ShutdownReboot:
        reboot();
        return EXIT_REBOOT;
    default:
        halt();
        return EXIT_HALT;
    }
}
```

### Kernel Class (kernel.h)

```cpp
#include <circle/cputhrottle.h>
#include <circle/gpiomanager.h>
#include <circle/i2cmaster.h>
#include "circle_stdlib_app.h"

class CKernel : public CStdlibAppStdio
{
public:
    CKernel();
    bool Initialize();
    TShutdownMode Run();

private:
    CConfig m_Config;           // INI file parser
    CCPUThrottle m_CPUThrottle; // Thermal management
    CGPIOManager m_GPIOManager; // GPIO interrupt handling
    CI2CMaster m_I2CMaster;     // I2C bus (OLED + MCP23017)
    CSPIMaster *m_pSPIMaster;   // SPI bus (optional)
    CMySynth *m_pSynth;         // Your synth engine
};
```

### Kernel Constructor — Hardware Init Order

The order of member initialization matters:

```cpp
CKernel::CKernel() :
    CStdlibAppStdio{"myproject"},           // 1. Base class (name for logs)
    m_Config{&mFileSystem},                  // 2. Config (needs filesystem)
    m_GPIOManager{&mInterrupt},              // 3. GPIO (needs interrupt system)
    m_I2CMaster{CMachineInfo::Get()->        // 4. I2C (auto-detect bus)
        GetDevice(DeviceI2CMaster), true}
{
}
```

---

## 5. Makefile Structure

### Your Makefile (src/Makefile)

```makefile
CIRCLE_STDLIB_DIR = ../circle-stdlib
SYNTH_DIR = ../MySynth/src

OBJS = main.o kernel.o config.o mysynth.o userinterface.o \
       ui4row.o u8g2_hal_circle.o \
       u8g2/u8g2_setup.o u8g2/u8g2_font.o  # ... etc

EXTRACLEAN = $(OBJS) $(OBJS:.o=.d)
OPTIMIZE = -O3

include ./Rules.mk    # pulls in Circle's build system
```

### Your Rules.mk (src/Rules.mk)

```makefile
-include $(CIRCLE_STDLIB_DIR)/Config.mk

NEWLIBDIR   ?= $(CIRCLE_STDLIB_DIR)/install/$(NEWLIB_ARCH)
CIRCLEHOME  ?= $(CIRCLE_STDLIB_DIR)/libs/circle

include $(CIRCLEHOME)/Rules.mk   # ← brings in compiler flags, pattern rules

INCLUDE += \
    -I $(CIRCLE_STDLIB_DIR)/include \
    -I $(NEWLIBDIR)/include

LIBS += \
    $(NEWLIBDIR)/lib/libm.a \
    $(NEWLIBDIR)/lib/libc.a \
    $(NEWLIBDIR)/lib/libcirclenewlib.a \
    $(CIRCLEHOME)/addon/display/libdisplay.a \
    $(CIRCLEHOME)/addon/gpio/libgpio.a \
    $(CIRCLEHOME)/addon/Properties/libproperties.a \
    $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
    $(CIRCLEHOME)/lib/usb/libusb.a \
    $(CIRCLEHOME)/lib/input/libinput.a \
    $(CIRCLEHOME)/lib/sound/libsound.a \
    $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
    $(CIRCLEHOME)/lib/fs/libfs.a \
    $(CIRCLEHOME)/lib/libcircle.a

-include $(DEPS)
```

### What Circle's Rules.mk Provides
- Pattern rules: `%.o: %.cpp`, `%.o: %.c`, `%.o: %.S`
- Target rule: `$(TARGET).img: $(OBJS) $(LIBS)` → links → objcopy → binary
- Linker script: `circle.ld`
- Compiler flags: `-mcpu=cortex-a53`, `-O2`, `-ffreestanding`, etc.
- `make clean` target

---

## 6. Circle API Reference (Most Used)

### Logging

```cpp
#include <circle/logger.h>

LOGMODULE("mymodule");     // declare at file scope

LOGDBG("Debug: %d", val);
LOGNOTE("Info: %s", str);
LOGWARN("Warning");
LOGERR("Error: %d", code);
LOGPANIC("Fatal");         // halts the kernel
```

### I2C

```cpp
#include <circle/i2cmaster.h>

CI2CMaster m_I2C{1, true};  // bus 1, fast mode
m_I2C.Initialize();

// Write
u8 data[] = {reg, value};
m_I2C.Write(0x3C, data, sizeof(data));

// Read
u8 result;
m_I2C.Write(0x3C, &reg, 1);
m_I2C.Read(0x3C, &result, 1);
```

### GPIO

```cpp
#include <circle/gpiopin.h>

// Output pin
CGPIOPin resetPin(23, GPIOModeOutput);
resetPin.Write(HIGH);
resetPin.Write(LOW);

// Input pin with pull-up
CGPIOPin inputPin(16, GPIOModeInputPullUp);
unsigned value = inputPin.Read();
```

### Timer

```cpp
#include <circle/timer.h>

// Get system clock (microseconds)
unsigned ticks = CTimer::Get()->GetClockTicks();

// Delay
CTimer::Get()->MsDelay(100);    // milliseconds
CTimer::Get()->usDelay(500);    // microseconds
```

### SSD1306 Display

```cpp
#include <display/ssd1306device.h>

CSSD1306Device oled(&m_I2C, 128, 64, 0x3C, 0, false, false);
oled.Initialize();

// Get framebuffer for direct pixel rendering
u8 *fb = oled.GetFrameBuffer();  // 1024 bytes (128×64÷8)
memcpy(fb, myRenderedBuffer, 1024);
oled.WriteFrameBuffer();          // send to display
```

### MCP23017

```cpp
#include <gpio/mcp23017.h>

CMCP23017 mcp(&m_I2C, 0x20);
mcp.Initialize();
mcp.SetDirectionA(0xFF);    // all inputs
mcp.SetDirectionB(0xFF);    // all inputs
mcp.SetPullUpA(0xFF);       // all pull-ups enabled
mcp.SetPullUpB(0xFF);

// Read ports
u8 portA = mcp.ReadPortA();
u8 portB = mcp.ReadPortB();
```

### Audio (I2S)

```cpp
#include <circle/sound/i2ssoundbasedevice.h>

// Implement by overriding GetChunk():
class CMySound : public CI2SSoundBaseDevice
{
    unsigned GetChunk(u32 *pBuffer, unsigned nChunkSize) override
    {
        // Fill pBuffer with interleaved stereo samples
        // Return number of words written
    }
};
```

### FAT Filesystem

```cpp
#include <fatfs/ff.h>

FIL file;
FRESULT res = f_open(&file, "voices/bank001.syx", FA_READ);
if (res == FR_OK)
{
    UINT bytesRead;
    u8 buffer[4096];
    f_read(&file, buffer, sizeof(buffer), &bytesRead);
    f_close(&file);
}
```

### Properties (INI File)

```cpp
#include <Properties/propertiesfatfsfile.h>

CPropertiesFatFsFile props("mysynth.ini", &fileSystem);
props.Load();

const char *device = props.GetString("SoundDevice", "i2s");
unsigned rate = props.GetNumber("SampleRate", 48000);
```

### Multi-Core

```cpp
#include <circle/multicore.h>

class CMyApp : public CMultiCoreSupport
{
    void Run(unsigned nCore) override
    {
        switch (nCore)
        {
        case 0: MainLoop(); break;     // Core 0: UI + management
        case 1: AudioLoop(); break;    // Core 1: Audio processing
        case 2: SynthCore2(); break;   // Core 2: Additional synth voices
        case 3: SynthCore3(); break;   // Core 3: Additional synth voices
        }
    }
};
```

---

## 7. SD Card Layout

The SD card must contain RPi boot firmware + your kernel + config files:

```
DREAMDEXED/                      ← FAT32 partition, volume label
├── bootcode.bin                 ← RPi stage 1 bootloader
├── start.elf                    ← RPi stage 2 (GPU firmware)
├── fixup.dat                    ← GPU memory split fix
├── bcm2710-rpi-3-b.dtb          ← Device tree blob (RPi3)
├── armstub8-rpi3.bin            ← AArch64 stub (RPi3 only)
├── kernel8.img                  ← YOUR APPLICATION (the only file you build)
├── config.txt                   ← RPi boot config (see below)
├── cmdline.txt                  ← Kernel command line
├── mysynth.ini                  ← Your runtime configuration
├── performance.ini              ← Performance/preset config
├── performance/                 ← Preset files
│   └── *.ini
└── sysex/                       ← Voice banks (optional)
    └── *.syx
```

### config.txt

```ini
# RPi boot configuration
arm_64bit=1
kernel=kernel8.img
kernel_address=0x80000
gpu_mem=32
dtoverlay=i2s-mmap
dtoverlay=disable-bt
core_freq=250
core_freq_min=250
enable_uart=1
```

### cmdline.txt

```
usbspeed=full
```

---

## 8. Linker Script (circle.ld)

Circle uses a minimal linker script:

```ld
ENTRY(_start)

SECTIONS
{
    .init : { *(.init) }           /* Boot code at LOADADDR (0x80000) */
    .text : { *(.text*) }          /* All code */
    .rodata : { *(.rodata*) }      /* Constants, strings */
    .init_array : {                /* C++ static constructors */
        __init_start = .;
        KEEP(*(.init_array*))
        __init_end = .;
    }
    .data : { *(.data*) }          /* Initialized globals */
    .bss : {                       /* Zero-initialized globals */
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        _end = .;
    }
}
```

**Key points:**
- `.init` section starts at `0x80000` (set via `--section-start=.init=`)
- `.init_array` holds C++ static constructors (called before `main()`)
- No `.heap` or `.stack` sections — Circle manages these dynamically
- Static constructors run BEFORE `CTimer` is initialized — don't use timers there

---

## 9. Forked circle-stdlib

DreamDexed uses a **forked** circle-stdlib that adds custom drivers:

| Driver | Location | Purpose |
|--------|----------|---------|
| `CSSD1309Display` | `addon/display/ssd1309display.*` | SSD1309 OLED with hardware reset |
| `CMCP23017` | `addon/gpio/mcp23017.*` | MCP23017 I2C I/O expander |

These are contributed upstream-compatible additions. If your synth project
uses the same hardware, you should use this fork (or merge the drivers into
your own circle fork).

**Fork URL**: See `.gitmodules` for the exact URL.

### Pinning circle to a specific commit

In `submod.sh`:
```bash
cd circle-stdlib/libs/circle
git checkout -f --recurse-submodules <commit-hash>
```

This ensures your build is reproducible and doesn't break with upstream changes.

---

## 10. Porting a New Synth — Step by Step

### 1. Create project structure

```bash
mkdir MyNewSynth && cd MyNewSynth
git init
git submodule add https://github.com/YourFork/circle-stdlib circle-stdlib
git submodule update --init --recursive
mkdir src
```

### 2. Copy boilerplate files from DreamDexed

```
circle_stdlib_app.h  → src/     (app base class)
main.cpp             → src/     (entry point)
kernel.cpp/.h        → src/     (adapt to your synth)
config.cpp/.h        → src/     (adapt to your parameters)
Rules.mk             → src/     (adjust LIBS as needed)
Makefile              → src/     (set your OBJS list)
build.sh             → ./       (configure + build script)
local-ci.sh          → ./       (SD card assembly)
deploy-sdcard.sh     → ./       (deployment script)
config.txt           → src/     (RPi boot config)
```

### 3. Adapt `kernel.cpp`

Replace `CMiniDexed` with your synth class:

```cpp
// In Initialize():
m_pSynth = new CMySynth(&m_Config, &mInterrupt, &m_GPIOManager,
                          &m_I2CMaster, &mFileSystem);
if (!m_pSynth->Initialize())
    return false;

// In Run():
while (true)
{
    m_pUSB->UpdatePlugAndPlay();
    m_pSynth->Process(bUpdated);
    m_CPUThrottle.Update();
}
```

### 4. Adapt `Makefile`

List your `.o` files:
```makefile
OBJS = main.o kernel.o config.o mysynth.o userinterface.o \
       u8g2_hal_circle.o ui4row.o \
       u8g2/u8g2_setup.o u8g2/u8g2_font.o ...
```

### 5. Build and test

```bash
export PATH="/Applications/ArmGNUToolchain/15.2.rel1/aarch64-none-elf/bin:$PATH"
export RPI=3
./build.sh                    # configure + compile
./local-ci.sh                 # assemble SD card
yes | ./deploy-sdcard.sh      # deploy to SD card
```

---

## 11. Common Patterns

### INI Configuration Access

```cpp
// config.h
class CConfig
{
public:
    CConfig(FATFS *pFileSystem);
    void Load();

    const char *GetSoundDevice() const;
    unsigned GetSampleRate() const;
    bool Is4RowUI() const;

private:
    CPropertiesFatFsFile m_Properties;
    std::string m_SoundDevice;
    unsigned m_nSampleRate;
};

// config.cpp
void CConfig::Load()
{
    m_Properties.Load();
    m_SoundDevice = m_Properties.GetString("SoundDevice", "i2s");
    m_nSampleRate = m_Properties.GetNumber("SampleRate", 48000);
}
```

### Hardware Reset Pattern

```cpp
void ResetDevice(unsigned nGPIOPin)
{
    CGPIOPin pin(nGPIOPin, GPIOModeOutput);
    pin.Write(HIGH);
    CTimer::Get()->MsDelay(10);
    pin.Write(LOW);
    CTimer::Get()->MsDelay(10);
    pin.Write(HIGH);
    CTimer::Get()->MsDelay(100);
}
```

### I2C Polling Loop (for MCP23017)

```cpp
void CUserInterface::PollMCP()
{
    u8 portA = m_pMCP->ReadPortA();
    u8 portB = m_pMCP->ReadPortB();

    // Detect falling edges (button presses)
    u8 pressed = m_nLastPortA & ~portA;

    // Process encoders from portB
    // ... (quadrature decoding)

    m_nLastPortA = portA;
    m_nLastPortB = portB;
}
```

---

## 12. Gotchas & Debugging

| Problem | Cause | Fix |
|---------|-------|-----|
| No HDMI output at all | Kernel crashes before logger init | Check constructors, static init |
| HDMI shows "kernel panic" | Assert failure or null pointer | Check init order |
| Build succeeds, old kernel deployed | Missing `.c` file for `.o` in Makefile | Verify all source files exist |
| `make` link error hidden | `local-ci.sh` doesn't propagate errors | Check `make` output for `undefined reference` |
| Display blank after boot | Hardware reset not performed | Add reset sequence for GPIO 23/24 |
| Encoder direction reversed | Channel A/B swapped | Swap clock/data pin assignments |
| Static init crash | Constructor calls `CTimer` | Move timer-dependent code to `Initialize()` |
| USB MIDI not detected | Missing `UpdatePlugAndPlay()` in main loop | Add USB polling to Run() loop |
| Audio clicks/dropouts | Process() takes too long | Profile, move work to other cores |
| Kernel too large (> 4MB) | Large font tables, debug info | Strip unused code, check KERNEL_MAX_SIZE |
