# Akashic Trance Machines — Hardware & UI Platform Guide

> **Purpose**: This document describes the physical hardware platform used by
> Akashic Trance Machines synthesizer projects. It serves as a complete
> reference for porting any synthesizer software to this hardware.

---

## 1. Platform Overview

The platform is a custom **Raspberry Pi 3 HAT** (Hardware Attached on Top) that
provides a complete user interface for embedded synthesizers:

- **128×64 OLED display** (SSD1309/SSD1306 compatible, I2C)
- **4 rotary encoders** with push-click (quadrature, I2C via MCP23017)
- **3 navigation inputs** (Back, Up, Down — via buttons or nav encoder)
- **I2S audio output** (to external DAC)
- **MIDI input** (UART, 31250 baud)

All user I/O (display, encoders, buttons) is on the **I2C bus** via two chips.
No GPIO pins are used for button/encoder scanning — they are all handled by the
MCP23017 I/O expander.

### Physical Layout

Two hardware configurations are supported for the left-side navigation:

#### Option A: 3 Buttons
```
┌─────────────────────────────────────────────────────┐
│                                                     │
│   (empty)   ┌──────────────────────┐  [═══ ENC_1 ═] │  ← Top
│   [BACK ]   │                      │  [═══ ENC_2 ═] │
│   [ UP  ]   │   128×64 OLED        │  [═══ ENC_3 ═] │
│   [DOWN ]   │                      │  [═══ ENC_4 ═] │  ← Bottom
│             └──────────────────────┘                 │
│                                                     │
│   ┌──── Raspberry Pi 3 GPIO Header ────┐             │
│   └────────────────────────────────────┘             │
└─────────────────────────────────────────────────────┘
```

#### Option B: Nav Encoder
```
┌─────────────────────────────────────────────────────┐
│                                                     │
│             ┌──────────────────────┐  [═══ ENC_1 ═] │  ← Top
│  [═NAV═]    │                      │  [═══ ENC_2 ═] │
│  click=Back │   128×64 OLED        │  [═══ ENC_3 ═] │
│  twist=↑↓   │                      │  [═══ ENC_4 ═] │  ← Bottom
│             └──────────────────────┘                 │
│                                                     │
│   ┌──── Raspberry Pi 3 GPIO Header ────┐             │
│   └────────────────────────────────────┘             │
└─────────────────────────────────────────────────────┘
```

- **Left side**: 3 navigation inputs — Back, Up, Down (via buttons OR encoder)
- **Center**: OLED display
- **Right side**: 4 rotary encoders with click (top to bottom: ENC_1 → ENC_4)

> **Note**: There is **no Home button**. Pressing Back from the root menu does
> nothing. Press Back repeatedly from any submenu to return to the root.

---

## 2. I2C Bus

All peripherals share a single I2C bus on the Raspberry Pi 3 GPIO header.

| Function | RPi Pin (Physical) | GPIO (BCM) |
|:---------|:------------------:|:----------:|
| I2C SDA  | Pin 3              | GPIO 2     |
| I2C SCL  | Pin 5              | GPIO 3     |

### I2C Devices

| Device    | Chip     | Address | Function                         |
|-----------|----------|:-------:|----------------------------------|
| OLED      | SSD1309  | `0x3C`  | 128×64 monochrome OLED display   |
| I/O Expander | MCP23017 | `0x20` | 16 inputs: buttons + encoders |

> **Note**: The SSD1309 is register-compatible with SSD1306. Any SSD1306
> driver will work. The only difference is the hardware reset sequence.

---

## 3. Dedicated GPIO Pins

Only 4 GPIO pins are used directly by the HAT (beyond I2C):

| Function           | RPi Pin | GPIO (BCM) | Direction | Description                    |
|--------------------|:-------:|:----------:|:---------:|--------------------------------|
| OLED Reset         | 16      | GPIO 23    | Output    | Active-low hardware reset      |
| MCP23017 Reset     | 18      | GPIO 24    | Output    | Active-low hardware reset      |
| MCP23017 INTA      | 36      | GPIO 16    | Input     | Port A interrupt (falling edge)|
| MCP23017 INTB      | 38      | GPIO 20    | Input     | Port B interrupt (falling edge)|

### Hardware Reset Sequence

Both the OLED and MCP23017 must be hardware-reset during boot:

```
1. Drive reset pin HIGH   →  10ms
2. Drive reset pin LOW    →  10ms  (active reset)
3. Drive reset pin HIGH   → 100ms  (recovery)
```

This sequence must complete **before** any I2C communication with the device.

---

## 4. MCP23017 I/O Expander

The MCP23017 provides 16 GPIO pins split into two 8-bit ports.
Both ports are configured as **inputs with internal pull-ups enabled**.
All buttons and encoder pins are **active-low** (pressed/active = 0, idle = 1).

### Port A — Buttons & Encoder Clicks (Digital Inputs)

| Pin   | Bit | Function           | Physical Position |
|:-----:|:---:|:-------------------|:------------------|
| GPA0  | 0   | Encoder 4 click    | Bottom encoder    |
| GPA1  | 1   | Encoder 3 click    |                   |
| GPA2  | 2   | Encoder 2 click    |                   |
| GPA3  | 3   | Encoder 1 click    | Top encoder       |
| GPA4  | 4   | Nav: Down (btn) / Nav Enc click (enc) | |
| GPA5  | 5   | Nav: Up (btn) / Nav Enc Ch A (enc) | |
| GPA6  | 6   | Nav: Back (btn) / Nav Enc Ch B (enc) | |
| GPA7  | 7   | *(unused / reserved)* | |

> **Important**: The physical order is **reversed** relative to bit numbers.
> Top encoder click = GPA3 (bit 3), Bottom encoder click = GPA0 (bit 0).
>
> GPA4–GPA6 serve dual purpose depending on nav mode:
> - **Buttons mode**: 3 discrete buttons (Back, Up, Down)
> - **Encoder mode**: 1 nav encoder (quadrature on GPA5/GPA6, click on GPA4)

### Port B — Encoder Rotation (Quadrature Inputs)

Each encoder uses 2 pins (Channel A and Channel B) for quadrature decoding.

| Pin   | Bit | Function              |
|:-----:|:---:|:----------------------|
| GPB0  | 0   | Encoder 1 — Channel B |
| GPB1  | 1   | Encoder 1 — Channel A |
| GPB2  | 2   | Encoder 2 — Channel B |
| GPB3  | 3   | Encoder 2 — Channel A |
| GPB4  | 4   | Encoder 3 — Channel B |
| GPB5  | 5   | Encoder 3 — Channel A |
| GPB6  | 6   | Encoder 4 — Channel B |
| GPB7  | 7   | Encoder 4 — Channel A |

> **Note**: Channel A and B are swapped (B on even bit, A on odd bit).
> Software must read them accordingly for correct rotation direction.

---

## 5. Encoder Specifications

### Physical Characteristics
- **Type**: Incremental rotary encoder with push-button
- **Detents**: Produces **3 raw quadrature state changes per physical click**
- **Output**: Standard AB quadrature (Gray code)

### Quadrature Decoding

The quadrature state is a 2-bit value: `(Channel_A << 1) | Channel_B`

State transitions for **clockwise** rotation: `00 → 01 → 11 → 10 → 00`
State transitions for **counter-clockwise**: `00 → 10 → 11 → 01 → 00`

Direction lookup table (indexed by `[previous_state << 2 | current_state]`):
```
 0, -1,  1,  0,
 1,  0,  0, -1,
-1,  0,  0,  1,
 0,  1, -1,  0
```

### Step Accumulation

Because encoders produce 3 raw transitions per detent, software must accumulate
raw direction values and only emit a ±1 step when the threshold is reached:

```
accumulator += direction_from_lookup_table
if accumulator >= steps_per_detent:    emit +1, reset accumulator
if accumulator <= -steps_per_detent:   emit -1, reset accumulator
```

**Default steps per detent**: `3` (configurable)

---

## 6. Button Handling

### Edge Detection
Buttons are detected by comparing the current port state with the previous read.
Only **falling edges** (HIGH → LOW transitions) should trigger events:

```
pressed_bits = last_port_value & ~current_port_value
```

### Debouncing
Hardware debouncing is not needed for the MCP23017 — the I2C polling rate
(typically 1–5 kHz) provides natural debouncing. For software debouncing,
a simple 50ms cooldown timer per button is sufficient.

### Auto-Repeat (for navigation buttons)
When a navigation button (Up/Down) is held:
- **Initial delay**: 400ms before repeat begins
- **Repeat interval**: 120ms between repeated events
- Only Up and Down should auto-repeat
- Back and encoder clicks should NOT auto-repeat

---

## 7. Display — 128×64 OLED

### Hardware
- **Controller**: SSD1309 (SSD1306-compatible)
- **Resolution**: 128 × 64 pixels (monochrome)
- **Interface**: I2C at address `0x3C`
- **Color depth**: 1-bit (white on black)

### Memory Layout
The framebuffer is **1024 bytes** (128 × 64 ÷ 8).
Pixels are organized in **page mode**: each byte represents 8 vertical pixels
in a single column. Bit 0 is the top pixel, bit 7 is the bottom pixel.

### Rendering Approach
The recommended rendering approach is **double-buffered**:

1. Render the entire frame to a RAM buffer (1024 bytes)
2. Compare with the previously sent buffer (`memcmp`)
3. Only send to the display via I2C if the buffer changed
4. Copy the new buffer over the "last sent" buffer

This minimizes I2C traffic and prevents display flicker.

---

## 8. The 4-Row UI Paradigm

The 4-Row UI is the standard user interface for this hardware platform.
It's specifically designed for the physical layout of 3 nav inputs + 4 encoders.

### Core Concept

The display shows **4 rows of content** at a time, from a scrollable list.
Each row is 16 pixels tall. A scrollbar on the left indicates position.

```
┌──┬──────────────────────────┬──────┐
│▓▓│ Bank: 001-OASIS          │▶████ │  Row 1 (y: 0–15)
│▓▓│ Perf: 001-Dream          │▶████ │  Row 2 (y: 16–31)
│  │ Save                     │▶████ │  Row 3 (y: 32–47)
│  │ PCCH: Off                │      │  Row 4 (y: 48–63)
└──┴──────────────────────────┴──────┘
 ▓▓ = scrollbar (2px wide)
```

### Display Zones

| Zone       | X range   | Width | Content                    |
|------------|-----------|-------|----------------------------|
| Scrollbar  | 0–1       | 2px   | Proportional scroll position indicator |
| Text       | 3–121     | 119px | `Label: Value` or menu item name |
| Action     | 122–127   | 6px   | ▶ icon (white box with inverted arrow) |

### Row Pixel Positions

| Row | Pixel range | Text baseline Y | Action box Y |
|-----|:-----------:|:---------------:|:------------:|
| 1   | 0–15        | 14              | 1            |
| 2   | 16–31       | 30              | 17           |
| 3   | 32–47       | 46              | 33           |
| 4   | 48–63       | 62              | 49           |

### Font
**Bold proportional, 13px height** (equivalent to u8g2 `u8g2_font_t0_13b_tr`).
Text is drawn with transparency mode enabled (only white pixels are drawn).

### Row Types

| Type | Display Format | Encoder | Click Action |
|------|---------------|---------|--------------|
| **Property** | `Label: Value` | Changes value | Optional (toggle, trigger) |
| **Menu Item** | `Label` | None | Enter submenu |
| **Action** | `Label` | None | Execute command |
| **Read-only** | `Label: Value` | None | None |

### Action Indicator (▶ icon)

Rows with an action show a ▶ arrow on the right side (x: 122–127).
The indicator is drawn as a white filled box with an XOR'd arrow bitmap:

```cpp
// Action icon bitmap (4×7 pixels, ▶ arrow)
static const unsigned char icon[] = {0x01, 0x03, 0x07, 0x0f, 0x07, 0x03, 0x01};

// Drawing (per row):
drawBox(122, box_y, 6, 15);        // White background (drawColor=1)
drawXBM(123, icon_y, 4, 7, icon);  // Arrow with XOR (drawColor=2)
```

### Scrollbar

The scrollbar is 2 pixels wide at x: 0–1.

**Height calculation:**
```
if total_items <= 4:
    bar_height = 64  (full height, no scrolling)
else:
    bar_height = max(4, floor((4 / total_items) * 64))
```

**Position calculation:**
```
if total_items <= 4:
    bar_y = 0
else:
    bar_y = floor((scroll_index / (total_items - 4)) * (64 - bar_height))
```

---

## 9. Input Mapping

### Navigation Functions

Three navigation functions — **Back**, **Up**, **Down** — with two hardware modes:

#### Buttons Mode

| MCP Pin | Function | Description |
|:-------:|:--------:|-------------|
| GPA6    | Back     | Go up one menu level |
| GPA5    | Up (▲)   | Scroll menu up |
| GPA4    | Down (▼) | Scroll menu down |

#### Nav Encoder Mode

| MCP Pin   | Function | Description |
|:---------:|:--------:|-------------|
| GPA4 (click) | Back  | Go up one menu level |
| GPA5/GPA6 (twist CW)  | Down (▼) | Scroll menu down |
| GPA5/GPA6 (twist CCW)  | Up (▲)  | Scroll menu up |

> **There is no Home button.** Press Back repeatedly to return to root.

### Encoder-to-Row Mapping

Each encoder controls the corresponding display row:

| Physical Encoder | Controls | Rotation | Click |
|:----------------:|:--------:|----------|-------|
| ENC_1 (top)      | Row 1    | Change value | Trigger action |
| ENC_2            | Row 2    | Change value | Trigger action |
| ENC_3            | Row 3    | Change value | Trigger action |
| ENC_4 (bottom)   | Row 4    | Change value | Trigger action |

### Interaction Rules

| Input | Effect |
|-------|--------|
| **Back** | Go up one menu level. No effect at root. |
| **Up** | Decrease scroll position by 1 (if > 0). Auto-repeats when held. |
| **Down** | Increase scroll position by 1 (if < max). Auto-repeats when held. |
| **ENC_N rotation** | Change value of property on visible row N. No effect on menu items or read-only rows. |
| **ENC_N click** | Trigger action on visible row N: enter submenu, toggle, execute command. |

---

## 10. Selector Pattern

Many menus use a **selector** — a property at the top of the list that
determines the context for all items below it.

**Example**: The Voices menu has a `TG: TG1` selector. Changing it to `TG2`
refreshes all voice parameters below to show TG2's values.

**Behavior when selector changes:**
1. All items below the selector refresh to reflect the new context
2. Scroll position resets to show items immediately below the selector
3. The selector itself remains visible at its position

---

## 11. Menu Architecture

The menu system is a **tree of pages**. Each page contains an ordered list of
rows (properties, menu items, actions, or read-only values).

### Navigation Model
- **BACK** always goes up one level in the menu tree
- **Encoder click on a MenuItem** descends into a submenu
- There is no explicit "select" button — encoder click IS the select action
- Press BACK repeatedly to return to root

### Menu Page Structure

```
MenuPage {
    name: string
    rows: Row[]
    parent: MenuPage | null
}

Row {
    type: Property | MenuItem | Action | ReadOnly
    label: string
    value: string | null       // null for MenuItem/Action
    hasAction: bool            // shows ▶ indicator
    onEncoderRotate(direction) // for Property type
    onEncoderClick()           // for MenuItem/Action with hasAction=true
}
```

---

## 12. Audio & MIDI

### Audio Output
- **Interface**: I2S (directly on RPi GPIO header)
- **Sample rate**: 48000 Hz (configurable)
- **Bit depth**: 24-bit
- Alternative: PWM output (for headphone jack, lower quality)

### MIDI Input
- **Interface**: UART (GPIO 15 = RX)
- **Baud rate**: 31250 (standard MIDI)
- **USB MIDI**: Also supported via USB host port

---

## 13. Porting Checklist

When porting a new synthesizer project to this hardware:

### Hardware Drivers Needed
- [ ] **I2C master** — for OLED and MCP23017 communication
- [ ] **MCP23017 driver** — read Port A (buttons/clicks) and Port B (encoders)
- [ ] **SSD1306/1309 display driver** — 128×64 framebuffer over I2C
- [ ] **GPIO output** — for hardware reset pins (GPIO 23, 24)
- [ ] **I2S audio output** — for the DAC
- [ ] **UART input** — for MIDI reception

### Input System
- [ ] **MCP23017 polling loop** — read both ports at 1-5 kHz
- [ ] **Quadrature decoder** — for 4 encoders with step accumulation (threshold=3)
- [ ] **Falling-edge button detector** — for 3 nav buttons + 4 encoder clicks
- [ ] **Auto-repeat** — for Up/Down (400ms delay, 120ms interval)
- [ ] **Nav encoder mode** — optional quadrature nav with click=Back

### UI System
- [ ] **Menu tree** — hierarchical page structure with rows
- [ ] **4-row renderer** — scrollbar + text + action indicators
- [ ] **Scrolling** — with proportional scrollbar
- [ ] **Encoder-to-row mapping** — ENC_N controls row N
- [ ] **Selector pattern** — for context-dependent parameter pages

### Configuration
- [ ] **INI file parser** — for runtime configuration on SD card
- [ ] **Pin assignment config** — so hardware mapping is not hardcoded

---

## 14. Reference Configuration

```ini
# Hardware Reset GPIOs
MCPResetGPIO=24
OLEDResetGPIO=23

# I2C Addresses
MCPI2CAddress=0x20
SSD1306LCDI2CAddress=0x3C

# Display
SSD1306LCDWidth=128
SSD1306LCDHeight=64

# Input Device
UIInputDevice=mcp23017

# Encoder Detent Steps
EncoderPulsePerStep=3

# UI Mode
UIMode=4row

# Navigation mode: buttons (default) or encoder
4RowNavMode=buttons

# --- Navigation: Buttons mode (3 buttons, no Home) ---
4RowBtnBackPin=GPA6
4RowBtnUpPin=GPA5
4RowBtnDownPin=GPA4

# --- Navigation: Encoder mode (twist=scroll, click=back) ---
# 4RowNavMode=encoder
# 4RowNavEncClickPin=GPA4
# 4RowNavEncChAPin=GPA5
# 4RowNavEncChBPin=GPA6

# Encoder Click Pins (active-low on MCP23017 Port A)
4RowEncClk1Pin=GPA3
4RowEncClk2Pin=GPA2
4RowEncClk3Pin=GPA1
4RowEncClk4Pin=GPA0

# Encoder Rotation Pins (quadrature on MCP23017 Port B)
4RowEnc1ClkPin=GPB0
4RowEnc1DataPin=GPB1
4RowEnc2ClkPin=GPB2
4RowEnc2DataPin=GPB3
4RowEnc3ClkPin=GPB4
4RowEnc3DataPin=GPB5
4RowEnc4ClkPin=GPB6
4RowEnc4DataPin=GPB7
```
