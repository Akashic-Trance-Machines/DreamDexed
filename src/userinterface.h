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
#ifndef _userinterface_h
#define _userinterface_h

#include "config.h"
#include "uimenu.h"
#include "uibuttons.h"
#include "mcp23017.h"
#include <sensor/ky040.h>
#include <display/hd44780device.h>
#include <display/ssd1306device.h>
#include <display/st7789device.h>
#include <circle/gpiomanager.h>
#include <circle/gpiopin.h>
#include <circle/writebuffer.h>
#include <circle/i2cmaster.h>
#include <circle/spimaster.h>

class CMiniDexed;

// Global volatile flags for MCP23017 interrupt handling (set in ISR, cleared in Process)
extern volatile bool g_bMCPInterruptA;
extern volatile bool g_bMCPInterruptB;

class CUserInterface
{
public:
	CUserInterface (CMiniDexed *pMiniDexed, CGPIOManager *pGPIOManager, CI2CMaster *pI2CMaster, CSPIMaster *pSPIMaster, CConfig *pConfig);
	~CUserInterface (void);

	bool Initialize (void);

	void LoadDefaultScreen ();

	void Process (void);

	void ParameterChanged (void);
	void DisplayChanged (void);

	// Write to display in this format:
	// +----------------+
	// |PARAM       MENU|
	// |[<]VALUE     [>]|
	// +----------------+
	void DisplayWrite (const char *pMenu, const char *pParam, const char *pValue,
			   bool bArrowDown, bool bArrowUp);

	// To be called from the MIDI device on reception of a MIDI CC message
	void UIMIDICmdHandler (unsigned nMidiCh, unsigned nMidiType, unsigned nMidiData1, unsigned nMidiData2);

private:
	void LCDWrite (const char *pString);		// Print to optional HD44780 display

	void EncoderEventHandler (CKY040::TEvent Event);
	static void EncoderEventStub (CKY040::TEvent Event, void *pParam);
	void UIButtonsEventHandler (CUIButton::BtnEvent Event);
	static void UIButtonsEventStub (CUIButton::BtnEvent Event, void *pParam);
	void UISetMIDIButtonChannel (unsigned uCh);

	// MCP23017 input processing
	void ProcessMCPInput (void);
	void DecodeMCPEncoder (bool bEncA, bool bEncB);
	void ProcessMCPButtons (uint8_t nPortA, uint8_t nPortB);
	static void MCPInterruptHandlerA (void *pParam);
	static void MCPInterruptHandlerB (void *pParam);

private:
	CMiniDexed *m_pMiniDexed;
	CGPIOManager *m_pGPIOManager;
	CI2CMaster *m_pI2CMaster;
	CSPIMaster *m_pSPIMaster;
	CConfig *m_pConfig;

	CCharDevice    *m_pLCD;
	CHD44780Device *m_pHD44780;
	CSSD1306Device *m_pSSD1306;
	CST7789Display *m_pST7789Display;
	CST7789Device  *m_pST7789;
	CWriteBufferDevice *m_pLCDBuffered;
	
	CUIButtons *m_pUIButtons;

	unsigned m_nMIDIButtonCh;

	CKY040 *m_pRotaryEncoder;
	bool m_bSwitchPressed;

	// MCP23017 support
	CMCP23017 *m_pMCP;
	CGPIOPin *m_pMCPInterruptPinA;
	CGPIOPin *m_pMCPInterruptPinB;
	uint8_t m_nMCPPortA;			// Cached port A state
	uint8_t m_nMCPPortB;			// Cached port B state
	uint8_t m_nMCPLastAB;			// Last encoder A/B state (2 bits)
	uint8_t m_nMCPLastPortA;		// Previous port A for edge detection
	uint8_t m_nMCPLastPortB;		// Previous port B for edge detection
	int m_nMCPEncoderSteps;			// Step accumulator for pulse-per-step divider
	
	// MCP button timing for click/doubleclick/longpress
	unsigned m_nMCPButtonPressTime;	// Tick when button was pressed
	unsigned m_nMCPButtonClickTime;	// Tick when last click ended (for double-click)
	unsigned m_nMCPButtonClicks;	// Click counter for double-click detection
	bool m_bMCPButtonHeld;			// True if button is currently held

	CUIMenu m_Menu;
};

#endif
