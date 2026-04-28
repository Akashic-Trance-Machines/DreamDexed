//
// ui4row.h
//
// DreamDexed - 4-Row Hierarchical UI Engine
// Copyright (C) 2024  The DreamDexed Team
//
// See 4RowUI.md for the full specification.
//
#pragma once

#include <cstdint>
#include <cstring>

extern "C" {
#include "u8g2/u8g2.h"
}

class CUserInterface;
class CMiniDexed;
class CConfig;
class CSSD1306Device;

// u8g2 HAL callbacks (implemented in u8g2_hal_circle.cpp)
extern "C" uint8_t u8x8_byte_circle_noop(u8x8_t *u8x8, uint8_t msg,
                                          uint8_t arg_int, void *arg_ptr);
extern "C" uint8_t u8x8_gpio_and_delay_circle(u8x8_t *u8x8, uint8_t msg,
                                               uint8_t arg_int, void *arg_ptr);

class CUI4Row
{
public:
	CUI4Row(CUserInterface *pUI, CMiniDexed *pMiniDexed, CConfig *pConfig);

	// --- Navigation events ---
	void OnBack();
	void OnScrollUp();
	void OnScrollDown();

	// --- Encoder events ---
	// nEncoder: 0–3 (maps to Row 1–4 on current viewport)
	void OnEncoderRotate(unsigned nEncoder, int nDirection);
	void OnEncoderClick(unsigned nEncoder);
	void OnEncoderLongHold(unsigned nEncoder); // held for backspace in text input

	// --- Display rendering ---
	// Call this to render current menu state to the SSD1306 framebuffer
	void Render(CSSD1306Device *pDisplay);

	// Called when an external param change requires a display refresh
	void OnParameterChanged();

	// Get display-dirty flag (true if Render() should be called)
	bool NeedsRedraw() const { return m_bDirty; }

private:
	// --- Row type definitions ---
	enum TRowType
	{
		RowTypeNone = 0,      // blank row
		RowTypeMenuItem,      // submenu entry (encoder click enters submenu)
		RowTypeProperty,      // encoder-adjustable value + optional action
		RowTypeReadOnly,      // label + value, no interaction (Status)
		RowTypeAction,        // action-only row (Save, Confirm, Cancel)
	};

	// Action types for the right column (all render the same ▶ icon)
	enum TActionType
	{
		ActionNone = 0,       // no action, no indicator drawn
		ActionEnterSubmenu,   // enter submenu on click
		ActionToggle,         // toggle ON/OFF or M/UM on click
		ActionCommand,        // execute command on click (Save, Delete, etc.)
	};

	// A single menu row definition
	struct TMenuRow
	{
		TRowType    Type;
		const char *pLabel;         // "Volume", "Performance", etc.
		const char *pValue;         // "100", "TG1", etc. (can be empty)
		TActionType Action;
	};

	// --- Menu page definition ---
	static const unsigned MAX_ROWS = 32;  // max items per menu page

	struct TMenuPage
	{
		const char *pTitle;            // page title (for debug)
		unsigned    nRowCount;         // total rows
		TMenuRow    Rows[MAX_ROWS];
	};

	// Build the current menu page based on state
	void BuildCurrentPage();

	// Build specific menu pages
	void BuildHomePage();
	void BuildPerformancePage();
	void BuildStatusPage();
	void BuildSaveSubmenuPage();
	void BuildDeleteConfirmPage();
	void BuildTextInputPage();

	// User bank helpers
	bool IsUserBank() const; // true when selected bank is 000_user (index 0)

	// Phase 3: Voices menu pages
	void BuildVoicesPage();
	void BuildVoicesPitchBendPage();
	void BuildVoicesPortamentoPage();
	void BuildVoicesNoteLimitPage();
	void BuildVoicesModulationPage();
	void BuildVoicesMIDIPage();
	void BuildVoicesEQPage();
	void BuildVoicesCompressorPage();
	void BuildVoicesEditVoicePage();
	void BuildVoicesOperatorsPage();

	// Phase 4: Effects menu pages
	void BuildEffectsPage();
	void BuildEffectsCS2InputPage();
	void BuildEffectsCS2MultitapPage();
	void BuildEffectsCS2EarlyDiffusionPage();
	void BuildEffectsCS2LateDiffusionPage();
	void BuildEffectsCS2LateLinesPage();
	void BuildEffectsCS2LowShelfPage();
	void BuildEffectsCS2HighShelfPage();
	void BuildEffectsCS2LowPassPage();

	// Effects helper: compute active FX chain index
	int GetActiveFxChainIndex() const;

	// Phase 5: Mixer page
	void BuildMixerPage();

	// --- Rendering helpers ---
	void RenderRow(unsigned nScreenRow, unsigned nItemIndex);
	void RenderScrollbar();

	// --- State ---
	CUserInterface *m_pUI;
	CMiniDexed     *m_pMiniDexed;
	CConfig        *m_pConfig;

	// u8g2 rendering instance (buffer-only mode)
	u8g2_t m_u8g2;

	// Menu navigation
	static const unsigned MAX_MENU_DEPTH = 8;
	unsigned m_nMenuDepth;                         // 0 = Home
	unsigned m_nMenuStack[MAX_MENU_DEPTH];         // which menu at each depth
	unsigned m_nScrollStack[MAX_MENU_DEPTH];       // scroll position at each depth

	unsigned m_nScrollIndex;   // first visible row index (0-based)
	bool     m_bDirty;         // needs redraw

	// Current page data
	TMenuPage m_CurrentPage;

	// Scratch buffers for formatted value strings
	// (pValue in TMenuRow points into these to avoid dangling pointers)
	static const unsigned VALUE_BUF_COUNT = 24; // Operators page needs 22+
	static const unsigned VALUE_BUF_LEN = 22;
	char m_szValueBuf[VALUE_BUF_COUNT][VALUE_BUF_LEN];

	// Performance sync state
	unsigned m_nSelectedPerformanceBankID;
	unsigned m_nSelectedPerformanceID;
	bool     m_bBankIsLoading;
	unsigned m_nLoadingFrameCount;

	// Text input state (Save-as-New naming screen)
	static const unsigned TEXT_INPUT_MAX_LEN = 14;
	char     m_szInputText[TEXT_INPUT_MAX_LEN + 1];
	unsigned m_nInputCursorPos;        // current cursor character position (0..13)
	bool     m_bInputIsCopy;           // true = Copy flow (pre-fills name)

	// Selector state (Phase 3+)
	unsigned m_nActiveTG;          // 0 to N-1 tone generators
	unsigned m_nActiveOP;          // 0-5 for Operators submenu
	unsigned m_nActiveModSource;   // 0=MW, 1=FC, 2=BC, 3=AT

	// Selector state (Phase 4: Effects)
	unsigned m_nActiveBus;         // 0 to Buses-1 (RPi4/5 multi-bus)
	unsigned m_nActiveFxBus;       // 0=SendFX1, 1=SendFX2, CConfig::MasterFX=MasterFX
	unsigned m_nActiveFxSlot;      // 0-2 (Slot1/2/3)

	// Mute state (Phase 5: Mixer)
	int  m_nSavedMasterVolume;     // saved vol before mute
	bool m_bMasterMuted;           // true if muted

	enum TMenuID
	{
		MenuHome = 0,
		MenuPerformance,
		MenuStatus,
		MenuSaveSubmenu,
		MenuDeleteConfirm,
		// Phase 3: Voices
		MenuVoices,
		MenuVoicesPitchBend,
		MenuVoicesPortamento,
		MenuVoicesNoteLimit,
		MenuVoicesModulation,
		MenuVoicesMIDI,
		MenuVoicesEQ,
		MenuVoicesCompressor,
		MenuVoicesEditVoice,
		MenuVoicesOperators,
		// Phase 4: Effects
		MenuEffects,
		MenuEffectsCS2Input,
		MenuEffectsCS2Multitap,
		MenuEffectsCS2EarlyDiffusion,
		MenuEffectsCS2LateDiffusion,
		MenuEffectsCS2LateLines,
		MenuEffectsCS2LowShelf,
		MenuEffectsCS2HighShelf,
		MenuEffectsCS2LowPass,
		// Save text input
		MenuTextInput,
		// Future phases:
		MenuMixer,
		MenuCount,
	};
};
