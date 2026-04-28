//
// userinterface.h
//
// MiniDexed - Dexed FM synthesizer for bare metal Raspberry Pi
// Copyright (C) 2022  The MiniDexed Team
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#pragma once

#include <cstdint>

#include <circle/gpiomanager.h>
#include <circle/gpiopin.h>
#include <circle/i2cmaster.h>
#include <circle/spimaster.h>
#include <circle/timer.h>
#include <circle/writebuffer.h>
#include <display/chardevice.h>
#include <display/hd44780device.h>
#include <display/ssd1306device.h>
#include <display/ssd1309display.h>
#include <display/st7789device.h>
#include <display/st7789display.h>
#include <gpio/mcp23017.h>
#include <sensor/ky040.h>

#include "config.h"
#include "uibuttons.h"
#include "uimenu.h"
#include "ui4row.h"

class CMiniDexed;

// MCP23017 pin descriptor
struct TMCPPin
{
	bool bValid;    // successfully parsed
	bool bIsPortA;  // true = Port A, false = Port B
	unsigned nBit;  // 0-7
};

// MCP23017 button → menu event binding
struct TMCPButtonBinding
{
	bool bIsPortA;
	unsigned nBit;
	CUIMenu::TMenuEvent Event;
};

// MCP23017 encoder binding (clock + data pins)
struct TMCPEncoderBinding
{
	bool bClockIsPortA;
	unsigned nClockBit;
	bool bDataIsPortA;
	unsigned nDataBit;
	uint8_t nLastState;    // last A/B quadrature state
	int nAccumulator;      // step accumulator
};

// MCP23017 encoder click binding (for 4-row mode)
struct TMCPEncoderClickBinding
{
	bool bIsPortA;
	unsigned nBit;
	unsigned nEncoderIndex; // 0-3 maps to row 1-4
};

class CUserInterface
{
public:
	CUserInterface(CMiniDexed *pMiniDexed, CGPIOManager *pGPIOManager, CI2CMaster *pI2CMaster, CSPIMaster *pSPIMaster, CConfig *pConfig);
	~CUserInterface();

	bool Initialize();

	void LoadDefaultScreen();

	void Process();

	void ParameterChanged();
	void DisplayChanged();

	// Write to display in this format:
	// +----------------+
	// |PARAM       MENU|
	// |[<]VALUE     [>]|
	// +----------------+
	void DisplayWrite(const char *pMenu, const char *pParam, const char *pValue,
			  bool bArrowDown, bool bArrowUp);

	// To be called from the MIDI device on reception of a MIDI CC message
	void UIMIDICmdHandler(int nMidiCh, uint8_t nMidiType, uint8_t nMidiData1, uint8_t nMidiData2);

private:
	void LCDWrite(const char *pString); // Print to optional HD44780 display

	void EncoderEventHandler(CKY040::TEvent Event);
	static void EncoderEventStub(CKY040::TEvent Event, void *pParam);
	void UIButtonsEventHandler(CUIButton::BtnEvent Event);
	static void UIButtonsEventStub(CUIButton::BtnEvent Event, void *pParam);
	void UISetMIDIButtonChannel(int nCh);

	// MCP23017 helpers
	static TMCPPin ParseMCPPin(const char *pStr);
	void AddMCPButtonBinding(const char *pPinStr, CUIMenu::TMenuEvent Event);
	void PollMCP();

private:
	CMiniDexed *m_pMiniDexed;
	CGPIOManager *m_pGPIOManager;
	CI2CMaster *m_pI2CMaster;
	CSPIMaster *m_pSPIMaster;
	CConfig *m_pConfig;

	CCharDevice *m_pLCD;
	CHD44780Device *m_pHD44780;
	CSSD1306Device *m_pSSD1306;
	CSSD1309Display *m_pSSD1309;
	CST7789Display *m_pST7789Display;
	CST7789Device *m_pST7789;
	CWriteBufferDevice *m_pLCDBuffered;

	CUIButtons *m_pUIButtons;

	int m_nMIDIButtonCh;

	CKY040 *m_pRotaryEncoder;
	bool m_bSwitchPressed;

	// MCP23017 I/O Expander
	CMCP23017 *m_pMCP;
	bool m_bUseMCP;

	// MCP23017 config-driven button bindings
	static const unsigned MAX_MCP_BUTTONS = 16;
	TMCPButtonBinding m_MCPButtons[MAX_MCP_BUTTONS];
	unsigned m_nMCPButtonCount;

	// MCP23017 config-driven encoder bindings
	// Slots 0-3: 4-row row encoders; slot 4 (optional): nav encoder
	static const unsigned MAX_MCP_ENCODERS = 5;
	TMCPEncoderBinding m_MCPEncoders[MAX_MCP_ENCODERS];
	unsigned m_nMCPEncoderCount;
	bool m_bNavEncoderMode;     // true = nav encoder (slot 4) instead of 3 nav buttons
	unsigned m_nNavEncoderIndex; // index in m_MCPEncoders for the nav encoder

	// MCP23017 port state tracking
	uint8_t m_nLastPortA;
	uint8_t m_nLastPortB;

	CUIMenu m_Menu;

	// 4-Row hierarchical UI (alternative mode)
	CUI4Row *m_pUI4Row;
	bool m_bUse4RowUI;

	// 4-Row encoder click bindings (MCP pin → encoder index)
	static const unsigned MAX_ENCODER_CLICKS = 4;
	TMCPEncoderClickBinding m_MCPEncoderClicks[MAX_ENCODER_CLICKS];
	unsigned m_nMCPEncoderClickCount;
};
