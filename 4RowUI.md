# DreamDexed 4-Row Hierarchical UI Specification

A second UI mode for 128×64 OLED displays with 3 navigation inputs and 4 rotary encoders (with click).
Activated via `UIMode=4row` in `minidexed.ini`. The existing "classic" UI remains default and untouched.

---

## Hardware Inputs

### Navigation (Left Side — Configurable)

Three navigation inputs: **UP**, **DOWN**, and **BACK**. There is no HOME button. Pressing BACK from the Home page does nothing. Pressing BACK from any submenu walks up one menu level. Multiple BACK presses return to Home.

Two hardware configurations are supported via `4RowNavMode` in `minidexed.ini`:

#### Buttons Mode (`4RowNavMode=buttons`, default)

| Input | MCP23017 Pin | Function |
|-------|-------------|----------|
| BTN_BACK | GPA6 | Go up one menu level |
| BTN_UP | GPA5 | Scroll menu up |
| BTN_DOWN | GPA4 | Scroll menu down |
| *(unused)* | GPA7 | — |

#### Nav Encoder Mode (`4RowNavMode=encoder`)

| Input | MCP23017 Pin | Function |
|-------|-------------|----------|
| NAV_ENC Click | GPA4 | Go up one menu level (BACK) |
| NAV_ENC Ch A | GPA5 | Quadrature channel A |
| NAV_ENC Ch B | GPA6 | Quadrature channel B |
| *(unused)* | GPA7 | — |

Twist clockwise = scroll down, twist counter-clockwise = scroll up.

### Value Encoders (Right Side — Unchanged)

| Input | Function |
|-------|----------|
| ENC_1 – ENC_4 | Change value on Row 1–4 |
| ENC_CLK_1 – ENC_CLK_4 | Trigger action on Row 1–4 |

---

## Display Layout

The display is 128×64 pixels, divided into 3 zones:

```
┌──┬──────────────────────────┬──────┐
│▓▓│ Bank: 001-OASIS          │▶████ │  Row 1 (y: 0–15)
│▓▓│ Perf: 001-Dream          │▶████ │  Row 2 (y: 16–31)
│  │ Save                     │▶████ │  Row 3 (y: 32–47)
│  │ PCCH: Off                │      │  Row 4 (y: 48–63)
└──┴──────────────────────────┴──────┘
 ▓▓ = scrollbar (filled portion)
```

### Left Zone: Scrollbar (x: 0–1, 2px wide)

A solid white vertical bar indicates scroll position and list size.

**Scrollbar sizing:**
```
if (total_items <= 4):
    bar_height = 64                       // full height, no scrolling needed
else:
    bar_height = max(4, floor((4 / total_items) * 64))
```
Minimum bar height is **4px** to ensure visibility.

**Scrollbar position:**
```
if (total_items <= 4):
    bar_y = 0                             // bar fills entire height
else:
    bar_y = floor((scroll_index / (total_items - 4)) * (64 - bar_height))
```
- `scroll_index = 0` → bar at top
- `scroll_index = total_items - 4` → bar at bottom

**Examples:**

| Total items | Bar height | Scrollable range |
|------------|-----------|-----------------|
| 4 or fewer | 64px (full) | no scrolling |
| 5 | 51px | 0–1 |
| 8 | 32px | 0–4 |
| 16 | 16px | 0–12 |
| 23 | 11px | 0–19 |
| 64+ | 4px (min) | 0–N |

**Rendering:**
```cpp
pDisplay->DrawFilledRect(0, bar_y, 2, bar_height);   // 2px wide at x=0
```

### Middle Zone: Text Content (x: 3–121)

Displays `Label: Value` for properties, or just `Label` for menu items.

**Font**: Equivalent to `u8g2_font_t0_13b_tr` — bold 13px proportional font.

**Text positions:**

| Row | Text x | Text baseline y | Row pixel range |
|-----|--------|----------------|-----------------|
| 1 | 3 | 14 | 0–15 |
| 2 | 3 | 30 | 16–31 |
| 3 | 3 | 46 | 32–47 |
| 4 | 3 | 62 | 48–63 |

```cpp
pDisplay->DrawText(3, 14, "Bank: 001-OASIS");   // Row 1
pDisplay->DrawText(3, 30, "Perf: 001-Dream");   // Row 2
pDisplay->DrawText(3, 46, "Save");               // Row 3
pDisplay->DrawText(3, 62, "PCCH: Off");          // Row 4
```

### Right Zone: Action Indicator (x: 122–127, 6px wide)

All action types (enter submenu, toggle state, trigger command) use the **same visual**: a white filled background box with an inverted ▶ icon. The indicator is only drawn on rows that have an action. Rows without an action leave this zone black.

**Action icon bitmap** (▶ arrow, 4×7 pixels):
```cpp
static const unsigned char image_ActionIcon_bits[] = {0x01, 0x03, 0x07, 0x0f, 0x07, 0x03, 0x01};
```

**Per-row pixel positions:**

| Row | Background box | Icon position | Icon draw mode |
|-----|---------------|---------------|----------------|
| 1 | `drawBox(122, 1, 6, 15)` | `drawXBM(123, 5, 4, 7)` | XOR (drawColor=2) |
| 2 | `drawBox(122, 17, 6, 15)` | `drawXBM(123, 21, 4, 7)` | XOR (drawColor=2) |
| 3 | `drawBox(122, 33, 6, 15)` | `drawXBM(123, 37, 4, 7)` | XOR (drawColor=2) |
| 4 | `drawBox(122, 49, 6, 15)` | `drawXBM(123, 53, 4, 7)` | XOR (drawColor=2) |

The background box is drawn with normal mode (`drawColor=1`), then the icon is drawn with XOR mode (`drawColor=2`) so it appears as a dark arrow on the white background.

### Rendering Sequence

```cpp
// 1. Clear screen
pDisplay->Clear();

// 2. Enable font/bitmap transparency
pDisplay->SetFontMode(1);
pDisplay->SetBitmapMode(1);

// 3. Draw scrollbar
pDisplay->DrawFilledRect(0, bar_y, 2, bar_height);

// 4. For each visible row (0–3):
//    a. Draw text at (3, baseline_y)
//    b. If row has action:
//       - drawColor=1: draw background box
//       - drawColor=2: draw icon XBM
//       - drawColor=1: reset

// 5. Send buffer to display
pDisplay->SendBuffer();
```

### Short Menus
Menus with fewer than 4 items leave remaining rows blank. Encoders/clicks for blank rows do nothing. Scrollbar fills the full 64px height.

---

## Row Types

**MenuProperty** (`📝`): Has a label and an encoder-adjustable value. May optionally have an action (shown as ▶). Format: `Label: Value`.

**MenuItem** (`📁`): Has a label only (no encoder-adjustable value). Action is always "enter submenu" (▶ shown).

**RowTypeAction**: Action row (e.g., "Overwrite", "Confirm", "Cancel"). Shows ▶ icon; encoder click triggers the command.

**Read-only**: Has label + live value. No encoder control, no action indicator. Used only in Status menu.

---

## Interaction Logic

| Input | Effect |
|-------|--------|
| **BACK** | Go up one menu level. No effect at Home page. |
| **UP** | Decrease `scroll_index` by 1 (if > 0). |
| **DOWN** | Increase `scroll_index` by 1 (if < `total_items - 4`). |
| **ENC_N rotation** | Change the value of the property on visible row N. No effect on MenuItem, Action, or Read-only rows. |
| **ENC_CLK_N click** | Trigger the action on visible row N: MenuItem → enter submenu, Toggle → toggle state, Command → execute, No action → ignored. |

---

## Selector Pattern

Certain menus use a **selector** property at the top that determines the context for items below it:

| Menu | Selector | Controls |
|------|----------|----------|
| Voices | TG (1–N) | All TG-specific properties and submenus |
| Operators | OP (1–6) | All 22 operator parameters |
| Modulation | Source (MW/FC/BC/AT) | Range, Pitch, Amplitude, EG Bias |
| Effects | Fx bus (SendFX1/SendFX2/MasterFX) | Dry Level visibility, slot states, algorithm, params |
| Effects | Fx slot (1/2/3) | Algorithm + algorithm-specific params |
| Effects | Fx algorithm | Algorithm-specific params |
| Mixer (RPi4/5) | Bus (B1/B2/…) | Dry Level, FX Returns, Return |
| Performance | Bank | Available performances |

**Behavior**: When a selector value changes, all items **below** it refresh to reflect the new context and the scroll position resets to show items immediately below the selector.

---

## Complete Menu Tree

### Home
```
📁 Performance                              ⚡ ▶
📁 Voices                                   ⚡ ▶
📁 Effects                                  ⚡ ▶
📁 Mixer                                    ⚡ ▶
📁 Status                                   ⚡ ▶
```

---

### Performance
```
📝 Bank: 001-OASIS                          
📝 Perf: 001-Dream                          
📁 Save                                     ⚡ ▶
📁 Delete                                   ⚡ ▶
📝 PCCH: Off                                
📝 Design Filter: All                       
```

#### → Save
```
   Overwrite                                ⚡ ▶
   New                                      ⚡ ▶ (text input)
   Save as default                          ⚡ ▶
```

#### → Delete
```
   Delete "001-Dream"?                      
   Confirm                                  ⚡ ▶
   Cancel                                   ⚡ ▶
```

---

### Voices
```
📝 TG: TG1                                  ← selector
📝 Bank: 001-OASIS                          
📝 Voice: 001-BRASS1                        
📝 Volume: 100                              
📝 Pan: 64                                  
📝 FX1-Send: 0                              
📝 FX2-Send: 0                              
📝 Detune: 0                                
📝 Cutoff: 99                               
📝 Resonance: 0                             
📁 Pitch Bend                               ⚡ ▶
📁 Portamento                               ⚡ ▶
📁 Note Limit                               ⚡ ▶
📝 Poly/Mono: Poly                          
📝 TG-Link: Off                             
📁 Modulation                               ⚡ ▶
📁 MIDI                                     ⚡ ▶
📁 EQ                                       ⚡ ▶
📁 Compressor                               ⚡ ▶
📁 Edit Voice                               ⚡ ▶
```

#### → Pitch Bend
```
📝 Bend Range: 2                            
📝 Bend Step: 0                             
```

#### → Portamento
```
📝 Mode: Fingered                           
📝 Glissando: Off                           
📝 Time: 0                                  
```

#### → Note Limit
```
📝 Limit Low: C-2                           
📝 Limit High: G8                           
📝 Shift: 0                                 
```

#### → Modulation (selector approach)
```
📝 Source: Mod. Wheel                        ← selector
📝 Range: 50                                
📝 Pitch: On                                
📝 Amplitude: Off                           
📝 EG Bias: On                              
```
Sources: Mod. Wheel, Foot Control, Breath Control, Aftertouch.

#### → MIDI
```
📝 Channel: 1                               
📝 SysEx Channel: 1                         
📝 SysEx Enable: Off                        
📝 Sustain Rx: On                           
📝 Portamento Rx: On                        
📝 Sostenuto Rx: On                         
📝 Hold2 Rx: On                             
```

#### → EQ (per-TG)
```
📝 Low Level: 0 dB                          
📝 Mid Level: 0 dB                          
📝 High Level: 0 dB                         
📝 Gain: 0 dB                               
📝 Low-Mid Freq                             
📝 Mid-High Freq                            
📝 Pre Lowcut                               
📝 Pre Highcut                              
```

#### → Compressor (per-TG)
```
📝 Enable: Off                              
📝 Pre Gain: 0 dB                           
📝 Threshold: -20 dBFS                      
📝 Ratio: 4:1                               
📝 Attack: 10 ms                            
📝 Release: 100 ms                          
📝 Makeup Gain: 0 dB                        
```

#### → Edit Voice
```
📝 Algorithm: 1                             
📝 Feedback: 0                              
📁 Operators                                ⚡ ▶
📝 P EG Rate 1: 99                          
📝 P EG Rate 2: 99                          
📝 P EG Rate 3: 99                          
📝 P EG Rate 4: 99                          
📝 P EG Level 1: 50                         
📝 P EG Level 2: 50                         
📝 P EG Level 3: 50                         
📝 P EG Level 4: 50                         
📝 Osc Key Sync: On                         
📝 LFO Speed: 0                             
📝 LFO Delay: 0                             
📝 LFO PMD: 0                               
📝 LFO AMD: 0                               
📝 LFO Sync: Off                            
📝 LFO Wave: Triangle                       
📝 P Mod Sens.: 0                           
📝 Transpose: C3                            
   Name                                     ⚡ ▶ (text input)
```

#### → Operators (selector approach)
```
📝 OP: OP1                                  ← selector
📝 Output Level: 99                         
📝 Freq Coarse: 1                           
📝 Freq Fine: 0                             
📝 Osc Detune: 7                            
📝 Osc Mode: Ratio                          
📝 EG Rate 1: 99                            
📝 EG Rate 2: 99                            
📝 EG Rate 3: 99                            
📝 EG Rate 4: 99                            
📝 EG Level 1: 99                           
📝 EG Level 2: 99                           
📝 EG Level 3: 99                           
📝 EG Level 4: 0                            
📝 Break Point: C3                          
📝 L Key Depth: 0                           
📝 R Key Depth: 0                           
📝 L Key Scale: -Lin                        
📝 R Key Scale: -Lin                        
📝 Rate Scaling: 0                          
📝 A Mod Sens.: 0                           
📝 K Vel. Sens.: 0                          
📝 Enable: On                               
```

---

### Effects

The Effects menu uses three cascading selectors. Available items change dynamically.

**SendFX selected (Dry Level visible):**
```
📝 Dry Level: 100                           
📝 Fx bus: SendFX1                          ⚡ ▶ (FX chain bypass toggle)  ← selector
📝 Fx slot: Slot1                           ⚡ ▶ (algo bypass toggle)      ← selector
📝 Fx: ZynDistortion                        ← selector
--- dynamic: algorithm-specific params ---
📝 Load Preset: 1                           
📝 Mix: 64                                  
📝 [... more algorithm params ...]          
📝 Bypass: Off                              
```

**MasterFX selected (Dry Level hidden):**
```
📝 Fx bus: MasterFX                         ⚡ ▶ (FX chain bypass toggle)
📝 Fx slot: Slot1                           ⚡ ▶ (algo bypass toggle)
📝 Fx: PlateReverb                          
📝 Mix Dry:Wet: 50                          
📝 [... algorithm params ...]              
📝 Bypass: Off                              
```

**Fx: None selected →** no parameters below.

**Fx bus options**: SendFX1, SendFX2, MasterFX (RPi3). RPi4/5 adds a Bus selector above Dry Level.

**Bypass levels**:
- Fx bus row action (▶ click) → toggles `FX::Parameter::Bypass` (entire FX chain on/off)
- Fx slot row action (▶ click) → toggles per-algorithm bypass (e.g., `ZynDistortionBypass`)

#### Available Effect Algorithms

| Algorithm | Params | Has sub-pages |
|-----------|--------|---------------|
| None | 0 | No |
| ZynDistortion | 15 | No |
| YKChorus | 6 | No |
| ZynChorus | 14 | No |
| ZynSympathetic | 17 | No |
| ZynAPhaser | 17 | No |
| ZynPhaser | 14 | No |
| DreamDelay | 9 | No |
| PlateReverb | 7 | No |
| CloudSeed2 | 9 top-level + 8 sub-pages | Yes |
| Compressor | 8 | No |
| EQ | 9 | No |

#### CloudSeed2 Sub-pages

When CloudSeed2 is selected, these appear as submenu items within the scrollable list:

```
📁 Input                                    ⚡ ▶
📁 Multitap Delay                           ⚡ ▶
📁 Early Diffusion                          ⚡ ▶
📁 Late Diffusion                           ⚡ ▶
📁 Late Lines                               ⚡ ▶
📁 Low Shelf                                ⚡ ▶
📁 High Shelf                               ⚡ ▶
📁 Low Pass                                 ⚡ ▶
```

---

### Mixer

**RPi3 (1 bus):**
```
📝 Master Vol: 100                          ⚡ ▶ (mute toggle)
📝 Dry Level: 100                           
📝 FX1 Return: 50                           
📝 FX2 Return: 50                           
📝 Return: 100                              
```

**RPi4/5 (multi-bus):**
```
📝 Master Vol: 100                          ⚡ ▶ (mute toggle)
📝 Bus: B1                                  ← selector
📝 Dry Level: 100                           
📝 FX1 Return: 50                           
📝 FX2 Return: 50                           
📝 Return: 100                              
```

**Mute**: Click ▶ on Master Vol to toggle. Sets Master Volume to 0 on mute, restores previous value on unmute.

---

### Status
```
   CPU Temp: 45/80 C                        
   CPU Speed: 600/1200 MHz                  
   Net IP: 192.168.1.100                    
   Version: v1.0                            
```
All read-only. No encoder control. No actions. No ▶ indicators.

---

## Text Input Screen

Used for **Save → New** and **Edit Voice → Name**.

```
   Name:                                    
📝 MYPERFOR_                                ⚡ ▶ (OK)
📝 Char: M                                  ⚡ ▶ (backspace)
   _ANCE123                                 
```

- **ENC_2**: Move cursor position through the name
- **ENC_3**: Change character at cursor (A–Z, 0–9, space, symbols)
- **ENC_CLK_2**: Confirm name (OK)
- **ENC_CLK_3**: Delete character at cursor (backspace)

---

## Configuration (minidexed.ini)

```ini
# UI Mode: classic (default) or 4row
UIMode=4row

# Navigation input mode: buttons (default) or encoder
4RowNavMode=buttons

# --- Navigation: Buttons mode ---
4RowBtnBackPin=GPA6
4RowBtnUpPin=GPA5
4RowBtnDownPin=GPA4

# --- Navigation: Encoder mode ---
4RowNavEncClickPin=GPA4
4RowNavEncChAPin=GPA5
4RowNavEncChBPin=GPA6

# --- Value Encoder Clicks (unchanged) ---
4RowEncClk1Pin=GPA3
4RowEncClk2Pin=GPA2
4RowEncClk3Pin=GPA1
4RowEncClk4Pin=GPA0

# --- Value Encoder Rotation (unchanged) ---
4RowEnc1ClkPin=GPB0
4RowEnc1DataPin=GPB1
4RowEnc2ClkPin=GPB2
4RowEnc2DataPin=GPB3
4RowEnc3ClkPin=GPB4
4RowEnc3DataPin=GPB5
4RowEnc4ClkPin=GPB6
4RowEnc4DataPin=GPB7
```

The existing classic UI parameters remain untouched.

---

## Architecture

The 4-row UI is implemented as a **parallel UI engine** alongside the existing `CUIMenu`:

- `CUI4Row` handles all 4-row menu logic, rendering, and input routing
- `CUserInterface` selects either `CUIMenu` or `CUI4Row` based on the `UIMode` config
- Same display hardware (SSD1306/SSD1309), using existing `CSSD1306Device` driver
- Rendering matches the pixel-precise layout defined above (equivalent to u8g2 reference mockup)
- All data access goes through the same `CMiniDexed` backend — no synth engine changes needed
- New ini parameters prefixed with `4Row` to avoid conflicts with classic UI settings
