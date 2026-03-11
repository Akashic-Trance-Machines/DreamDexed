# ReBirth-Pie GPIO & MCP23017 Pinout Reference

## Raspberry Pi 3 GPIO Connections

| Physical Pin | GPIO (BCM) | Function | Direction |
|:---:|:---:|---|---|
| 3 | 2 | I2C SDA (OLED + MCP23017) | Bidirectional |
| 5 | 3 | I2C SCL (OLED + MCP23017) | Output |
| 16 | 23 | OLED Reset (`GPIO_OLED_RESET`) | Output |
| 18 | 24 | MCP23017 Reset (`GPIO_MCP_RESET`) | Output |
| 36 | 16 | MCP23017 INTA (`GPIO_MCP_INTA`) | Input, falling edge |
| 38 | 20 | MCP23017 INTB (`GPIO_MCP_INTB`) | Input, falling edge |

## I2C Devices

| Device | Address | Function |
|---|:---:|---|
| SSD1309 OLED | 0x3C | 128×64 OLED display |
| MCP23017 | 0x20 | 16-bit I/O expander (A0/A1/A2 = GND) |

## MCP23017 Port A — Buttons & Encoder Clicks

All Port A pins are configured as **inputs with internal pull-ups**. Buttons are **active-low** (pressed = 0).

| GPA Pin | Bit | Define | Function | Physical Position |
|:---:|:---:|---|---|---|
| GPA0 | 0 | `ENC_CLK_4` | Encoder 4 click | Bottom encoder |
| GPA1 | 1 | `ENC_CLK_3` | Encoder 3 click | |
| GPA2 | 2 | `ENC_CLK_2` | Encoder 2 click | |
| GPA3 | 3 | `ENC_CLK_1` | Encoder 1 click | Top encoder |
| GPA4 | 4 | `BTN_4` | Nav button → Mix page | Bottom button |
| GPA5 | 5 | `BTN_3` | Nav button → FX page | |
| GPA6 | 6 | `BTN_2` | Nav button → Voices page | |
| GPA7 | 7 | `BTN_1` | Nav button → Main page | Top button |

> **Note:** Both encoder clicks and nav buttons are physically wired in reverse order relative to their GPA pin numbers (top = highest GPA for buttons, lowest GPA for encoder clicks).

## MCP23017 Port B — Encoder Rotation

All Port B pins are configured as **inputs with internal pull-ups**. Each encoder uses two pins (A and B channels). The A/B reads are swapped in software to match the correct rotation direction.

| GPB Pin | Bit | Function |
|:---:|:---:|---|
| GPB0 | 0 | Encoder 1 — Channel B (top encoder) |
| GPB1 | 1 | Encoder 1 — Channel A |
| GPB2 | 2 | Encoder 2 — Channel B |
| GPB3 | 3 | Encoder 2 — Channel A |
| GPB4 | 4 | Encoder 3 — Channel B |
| GPB5 | 5 | Encoder 3 — Channel A |
| GPB6 | 6 | Encoder 4 — Channel B (bottom encoder) |
| GPB7 | 7 | Encoder 4 — Channel A |

## Encoder Configuration

Encoders produce **3 raw quadrature steps per physical detent**. This is configurable via `rebirth-pie.ini`:

```ini
encoder_steps=3
```

Valid range: 1–12. The encoder accumulator in `encoder.cpp` collects raw steps and only emits ±1 when the threshold is reached.

## Reset Sequence

Both the OLED and MCP23017 are hardware-reset during initialization:

1. **HIGH** for 10ms
2. **LOW** for 10ms (active reset)
3. **HIGH** for 100ms (recovery)

## Key Source Files

| File | Purpose |
|---|---|
| `src/kernel.h` | GPIO pin defines |
| `src/ui/ButtonManager.h` | Button/encoder click pin mapping |
| `src/ui/encoder.cpp` | Encoder rotation reading & accumulator |
| `src/ui/mcp23017.cpp` | MCP23017 I2C driver & reset |
| `src/ui/ssd1309.cpp` | OLED I2C driver & reset |
| `src/CoreManager.cpp` | Core 2 polling loop (encoders + buttons) |
