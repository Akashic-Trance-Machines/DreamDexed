//
// userinterface.cpp
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
#include "userinterface.h"
#include "minidexed.h"
#include <circle/logger.h>
#include <circle/string.h>
#include <circle/startup.h>
#include <cstring>
#include <cassert>

LOGMODULE ("ui");

// Global volatile flags for MCP23017 interrupt handling
volatile bool g_bMCPInterruptA = false;
volatile bool g_bMCPInterruptB = false;

CUserInterface::CUserInterface (CMiniDexed *pMiniDexed, CGPIOManager *pGPIOManager, CI2CMaster *pI2CMaster, CSPIMaster *pSPIMaster, CConfig *pConfig)
:	m_pMiniDexed (pMiniDexed),
	m_pGPIOManager (pGPIOManager),
	m_pI2CMaster (pI2CMaster),
	m_pSPIMaster (pSPIMaster),
	m_pConfig (pConfig),
	m_pLCD (0),
	m_pLCDBuffered (0),
	m_pUIButtons (0),
	m_pRotaryEncoder (0),
	m_bSwitchPressed (false),
	m_pMCP (0),
	m_pMCPInterruptPinA (0),
	m_pMCPInterruptPinB (0),
	m_nMCPPortA (0xFF),
	m_nMCPPortB (0xFF),
	m_nMCPLastAB (0),
	m_nMCPLastPortA (0xFF),
	m_nMCPLastPortB (0xFF),
	m_Menu (this, pMiniDexed, pConfig)
{
}

CUserInterface::~CUserInterface (void)
{
	delete m_pRotaryEncoder;
	delete m_pUIButtons;
	delete m_pLCDBuffered;
	delete m_pLCD;
	delete m_pMCPInterruptPinB;
	delete m_pMCPInterruptPinA;
	delete m_pMCP;
}

bool CUserInterface::Initialize (void)
{
	assert (m_pConfig);

	if (m_pConfig->GetLCDEnabled ())
	{
		unsigned i2caddr = m_pConfig->GetLCDI2CAddress ();
		unsigned ssd1306addr = m_pConfig->GetSSD1306LCDI2CAddress ();
		bool st7789 = m_pConfig->GetST7789Enabled ();
		if (ssd1306addr != 0) {
			m_pSSD1306 = new CSSD1306Device (m_pConfig->GetSSD1306LCDWidth (), m_pConfig->GetSSD1306LCDHeight (),
											 m_pI2CMaster, ssd1306addr,
											 m_pConfig->GetSSD1306LCDRotate (), m_pConfig->GetSSD1306LCDMirror ());
			if (!m_pSSD1306->Initialize ())
			{
				LOGDBG("LCD: SSD1306 initialization failed");
				return false;
			}
			LOGDBG ("LCD: SSD1306");
			m_pLCD = m_pSSD1306;
		}
		else if (st7789)
		{
			if (m_pSPIMaster == nullptr)
			{
				LOGDBG("LCD: ST7789 Enabled but SPI Initialisation Failed");
				return false;
			}

			unsigned long nSPIClock = 1000 * m_pConfig->GetSPIClockKHz();
			unsigned nSPIMode = m_pConfig->GetSPIMode();
			unsigned nCPHA = (nSPIMode & 1) ? 1 : 0;
			unsigned nCPOL = (nSPIMode & 2) ? 1 : 0;
			LOGDBG("SPI: CPOL=%u; CPHA=%u; CLK=%u",nCPOL,nCPHA,nSPIClock);
			m_pST7789Display = new CST7789Display (m_pSPIMaster,
							m_pConfig->GetST7789Data(),
							m_pConfig->GetST7789Reset(),
							m_pConfig->GetST7789Backlight(),
							m_pConfig->GetST7789Width(),
							m_pConfig->GetST7789Height(),
							nCPOL, nCPHA, nSPIClock,
							m_pConfig->GetST7789Select());
			if (m_pST7789Display->Initialize())
			{
				extern const TFont DDFont8x16;
				extern const TFont DDFont12x22;

				m_pST7789Display->SetRotation (m_pConfig->GetST7789Rotation());

				bool bDoubleFont = m_pConfig->GetST7789FontSize() == 16 ? true : false;
				const TFont& font = m_pConfig->GetST7789FontSize() == 12 ? DDFont12x22 : DDFont8x16;

				m_pST7789 = new CST7789Device (m_pSPIMaster, m_pST7789Display, m_pConfig->GetLCDColumns (), m_pConfig->GetLCDRows (), font, bDoubleFont, bDoubleFont);
				if (m_pST7789->Initialize())
				{
					LOGDBG ("LCD: ST7789");
					m_pLCD = m_pST7789;
				}
				else
				{
					LOGDBG ("LCD: Failed to initalize ST7789 character device");
					delete (m_pST7789);
					delete (m_pST7789Display);
					m_pST7789 = nullptr;
					m_pST7789Display = nullptr;
					return false;
				}
			}
			else
			{
				LOGDBG ("LCD: Failed to initialize ST7789 display");
				delete (m_pST7789Display);
				m_pST7789Display = nullptr;
				return false;
			}
		}
		else if (i2caddr == 0)
		{
			m_pHD44780 = new CHD44780Device (m_pConfig->GetLCDColumns (), m_pConfig->GetLCDRows (),
							 m_pConfig->GetLCDPinData4 (),
							 m_pConfig->GetLCDPinData5 (),
							 m_pConfig->GetLCDPinData6 (),
							 m_pConfig->GetLCDPinData7 (),
							 m_pConfig->GetLCDPinEnable (),
							 m_pConfig->GetLCDPinRegisterSelect (),
							 m_pConfig->GetLCDPinReadWrite ());
			if (!m_pHD44780->Initialize ())
			{
				LOGDBG("LCD: HD44780 initialization failed");
				return false;
			}
			LOGDBG ("LCD: HD44780");
			m_pLCD = m_pHD44780;
		}
		else
		{
			m_pHD44780 = new CHD44780Device (m_pI2CMaster, i2caddr,
							m_pConfig->GetLCDColumns (), m_pConfig->GetLCDRows ());
			if (!m_pHD44780->Initialize ())
			{
				LOGDBG("LCD: HD44780 (I2C) initialization failed");
				return false;
			}
			LOGDBG ("LCD: HD44780 I2C");
			m_pLCD = m_pHD44780;
		}
		assert (m_pLCD);

		m_pLCDBuffered = new CWriteBufferDevice (m_pLCD);
		assert (m_pLCDBuffered);
		// clear sceen and go to top left corner
		LCDWrite ("\x1B[H\x1B[J");		// cursor home and clear screen
		LCDWrite ("\x1B[?25l\x1B""d+");		// cursor off, autopage mode
		LCDWrite ("MiniDexed\nLoading...");
		m_pLCDBuffered->Update ();

		LOGDBG ("LCD initialized");
	}

	m_pUIButtons = new CUIButtons (	m_pConfig );
	assert (m_pUIButtons);

	if (!m_pUIButtons->Initialize ())
	{
		return false;
	}

	m_pUIButtons->RegisterEventHandler (UIButtonsEventStub, this);
	UISetMIDIButtonChannel (m_pConfig->GetMIDIButtonCh ());

	LOGDBG ("Button User Interface initialized");

	// Only create CKY040 encoder if using GPIO pins (not MCP pins)
	// When using MCP pins, encoder is handled by ProcessMCPInput()
	unsigned nEncoderClock = m_pConfig->GetEncoderPinClock ();
	unsigned nEncoderData = m_pConfig->GetEncoderPinData ();
	
	if (m_pConfig->GetEncoderEnabled () && !IsMCPPin (nEncoderClock) && !IsMCPPin (nEncoderData))
	{
		m_pRotaryEncoder = new CKY040 (nEncoderClock,
					       nEncoderData,
					       m_pConfig->GetButtonPinShortcut (),
					       m_pGPIOManager);
		assert (m_pRotaryEncoder);

		if (!m_pRotaryEncoder->Initialize ())
		{
			return false;
		}

		m_pRotaryEncoder->RegisterEventHandler (EncoderEventStub, this);

		LOGDBG ("Rotary encoder initialized");
	}
	else if (m_pConfig->GetEncoderEnabled () && (IsMCPPin (nEncoderClock) || IsMCPPin (nEncoderData)))
	{
		LOGDBG ("Rotary encoder on MCP pins - handled by MCP interrupt");
	}

	// MCP23017 I/O Expander initialization
	if (m_pConfig->GetMCPEnabled ())
	{
		m_pMCP = new CMCP23017 (*m_pI2CMaster, m_pConfig->GetMCPAddress ());
		assert (m_pMCP);

		// Initialize Port A and B for UI inputs (all 8 bits each)
		if (!m_pMCP->Init_UI_PortA (0xFF))
		{
			LOGERR ("MCP23017 Port A initialization failed");
			return false;
		}
		if (!m_pMCP->Init_UI_PortB (0xFF))
		{
			LOGERR ("MCP23017 Port B initialization failed");
			return false;
		}

		// Setup GPIO interrupt pin for Port A
		unsigned nIntPinA = m_pConfig->GetMCPAInterruptGPIO ();
		if (nIntPinA > 0)
		{
			m_pMCPInterruptPinA = new CGPIOPin (nIntPinA, GPIOModeInputPullUp, m_pGPIOManager);
			m_pMCPInterruptPinA->ConnectInterrupt (MCPInterruptHandlerA, this);
			m_pMCPInterruptPinA->EnableInterrupt (GPIOInterruptOnFallingEdge);
			LOGDBG ("MCP23017 INTA on GPIO%u", nIntPinA);
		}

		// Setup GPIO interrupt pin for Port B
		unsigned nIntPinB = m_pConfig->GetMCPBInterruptGPIO ();
		if (nIntPinB > 0)
		{
			m_pMCPInterruptPinB = new CGPIOPin (nIntPinB, GPIOModeInputPullUp, m_pGPIOManager);
			m_pMCPInterruptPinB->ConnectInterrupt (MCPInterruptHandlerB, this);
			m_pMCPInterruptPinB->EnableInterrupt (GPIOInterruptOnFallingEdge);
			LOGDBG ("MCP23017 INTB on GPIO%u", nIntPinB);
		}

		// Read initial port states
		m_nMCPPortA = m_pMCP->ReadGpioA ();
		m_nMCPPortB = m_pMCP->ReadGpioB ();
		m_nMCPLastPortA = m_nMCPPortA;
		m_nMCPLastPortB = m_nMCPPortB;

		LOGDBG ("MCP23017 initialized at I2C address 0x%02X", m_pConfig->GetMCPAddress ());
	}

	m_Menu.EventHandler (CUIMenu::MenuEventUpdate);

	return true;
}

void CUserInterface::LoadDefaultScreen ()
{
	// performance load
	if (m_pConfig->GetDefaultScreen() == 1)
	{
		m_Menu.EventHandler (CUIMenu::MenuEventStepDown);
		m_Menu.EventHandler (CUIMenu::MenuEventSelect);
		m_Menu.EventHandler (CUIMenu::MenuEventSelect);
	}
}

void CUserInterface::Process (void)
{
	if (m_pLCDBuffered)
	{
		m_pLCDBuffered->Update ();
	}

	// Process MCP23017 interrupts if enabled
	if (m_pMCP)
	{
		ProcessMCPInput ();
	}

	if (m_pUIButtons)
	{
		m_pUIButtons->Update();
	}
}

void CUserInterface::ParameterChanged (void)
{
	m_Menu.EventHandler (CUIMenu::MenuEventUpdateParameter);
}

void CUserInterface::DisplayChanged (void)
{
	m_Menu.EventHandler (CUIMenu::MenuEventUpdate);
}

void CUserInterface::DisplayWrite (const char *pMenu, const char *pParam, const char *pValue,
				   bool bArrowDown, bool bArrowUp)
{
	assert (pMenu);
	assert (pParam);
	assert (pValue);

	size_t nLineMaxLen = m_pConfig->GetLCDColumns ();

	const char* pHdr = "\x1B[H\E[?25l"; // cursor home and off
	size_t nHdrLen = strlen(pHdr);

	const char* pClear = "\x1B[K"; // clear end of line
	size_t nClearLen = strlen(pClear);

	size_t nParamLen = std::min (nLineMaxLen, strlen (pParam));
	size_t nMenuLen = strlen (pMenu);
	size_t nFill1Len = nLineMaxLen > nParamLen + nMenuLen ?
		nLineMaxLen - nParamLen - nMenuLen : 1;

	nFill1Len = std:: min (nLineMaxLen - nParamLen, nFill1Len);
	nMenuLen = std::min (nLineMaxLen - nParamLen - nFill1Len, nMenuLen);

	size_t nLine1Len = nParamLen + nFill1Len + nMenuLen;

	size_t nArrowsLen = 2;
	size_t nValueLen = std::min (nLineMaxLen - nArrowsLen, strlen (pValue));
	size_t nFill2Len = bArrowUp ? nLineMaxLen - nArrowsLen - nValueLen : 0;
	size_t nLine2Len = nValueLen + nFill2Len + nArrowsLen;

	if (nLine2Len >= nLineMaxLen)
		nClearLen = 0;

	size_t nOffset = 0;

	char pLines[nHdrLen + nLine1Len  + nLine2Len + nClearLen + 1];

	memcpy (pLines, pHdr, nHdrLen);
	nOffset += nHdrLen;

	memcpy (pLines + nOffset, pParam, nParamLen);
	nOffset += nParamLen;

	memset (pLines + nOffset, ' ', nFill1Len);
	nOffset += nFill1Len;

	memcpy (pLines + nOffset, pMenu, nMenuLen);
	nOffset += nMenuLen;

	pLines[nOffset++] = bArrowDown ? '<' : ' ';

	memcpy (pLines + nOffset, pValue, nValueLen);
	nOffset += nValueLen;

	memset (pLines + nOffset, ' ', nFill2Len);
	nOffset += nFill2Len;

	pLines[nOffset++] = bArrowUp ? '>' : ' ';

	memcpy (pLines + nOffset, pClear, nClearLen);
	nOffset += nClearLen;

	pLines[nOffset++] = 0;

	LCDWrite ((const char *)pLines);
}

void CUserInterface::LCDWrite (const char *pString)
{
	if (m_pLCDBuffered)
	{
		m_pLCDBuffered->Write (pString, strlen (pString));
	}
}

void CUserInterface::EncoderEventHandler (CKY040::TEvent Event)
{
	switch (Event)
	{
	case CKY040::EventSwitchDown:
		m_bSwitchPressed = true;
		break;

	case CKY040::EventSwitchUp:
		m_bSwitchPressed = false;
		break;

	case CKY040::EventClockwise:
		if (m_bSwitchPressed) {
			// We must reset the encoder switch button to prevent events from being
			// triggered after the encoder is rotated
			m_pUIButtons->ResetButton(m_pConfig->GetButtonPinShortcut());
			m_Menu.EventHandler(CUIMenu::MenuEventPressAndStepUp);

		}
		else {
			m_Menu.EventHandler(CUIMenu::MenuEventStepUp);
		}
		break;

	case CKY040::EventCounterclockwise:
		if (m_bSwitchPressed) {
			m_pUIButtons->ResetButton(m_pConfig->GetButtonPinShortcut());
			m_Menu.EventHandler(CUIMenu::MenuEventPressAndStepDown);
		}
		else {
			m_Menu.EventHandler(CUIMenu::MenuEventStepDown);
		}
		break;

	case CKY040::EventSwitchHold:
		if (m_pRotaryEncoder->GetHoldSeconds () >= 120)
		{
			delete m_pLCD;		// reset LCD

			reboot ();
		}
		break;

	default:
		break;
	}
}

void CUserInterface::EncoderEventStub (CKY040::TEvent Event, void *pParam)
{
	CUserInterface *pThis = static_cast<CUserInterface *> (pParam);
	assert (pThis != 0);

	pThis->EncoderEventHandler (Event);
}

void CUserInterface::UIButtonsEventHandler (CUIButton::BtnEvent Event)
{
	switch (Event)
	{
	case CUIButton::BtnEventPrev:
		m_Menu.EventHandler (CUIMenu::MenuEventStepDown);
		break;

	case CUIButton::BtnEventNext:
		m_Menu.EventHandler (CUIMenu::MenuEventStepUp);
		break;

	case CUIButton::BtnEventBack:
		m_Menu.EventHandler (CUIMenu::MenuEventBack);
		break;

	case CUIButton::BtnEventSelect:
		m_Menu.EventHandler (CUIMenu::MenuEventSelect);
		break;

	case CUIButton::BtnEventHome:
		m_Menu.EventHandler (CUIMenu::MenuEventHome);
		break;

	case CUIButton::BtnEventPgmUp:
		m_Menu.EventHandler (CUIMenu::MenuEventPgmUp);
		break;

	case CUIButton::BtnEventPgmDown:
		m_Menu.EventHandler (CUIMenu::MenuEventPgmDown);
		break;

	case CUIButton::BtnEventBankUp:
		m_Menu.EventHandler (CUIMenu::MenuEventBankUp);
		break;

	case CUIButton::BtnEventBankDown:
		m_Menu.EventHandler (CUIMenu::MenuEventBankDown);
		break;

	case CUIButton::BtnEventTGUp:
		m_Menu.EventHandler (CUIMenu::MenuEventTGUp);
		break;

	case CUIButton::BtnEventTGDown:
		m_Menu.EventHandler (CUIMenu::MenuEventTGDown);
		break;

	default:
		break;
	}
}

void CUserInterface::UIButtonsEventStub (CUIButton::BtnEvent Event, void *pParam)
{
	CUserInterface *pThis = static_cast<CUserInterface *> (pParam);
	assert (pThis != 0);

	pThis->UIButtonsEventHandler (Event);
}

void CUserInterface::UIMIDICmdHandler (unsigned nMidiCh, unsigned nMidiType, unsigned nMidiData1, unsigned nMidiData2)
{
	if (m_nMIDIButtonCh == CMIDIDevice::Disabled)
	{
		// MIDI buttons are not enabled
		return;
	}
	if ((m_nMIDIButtonCh != nMidiCh) && (m_nMIDIButtonCh != CMIDIDevice::OmniMode))
	{
		// Message not on the MIDI Button channel and MIDI buttons not in OMNI mode
		return;
	}
	
	if (m_pUIButtons)
	{
		m_pUIButtons->BtnMIDICmdHandler (nMidiType, nMidiData1, nMidiData2);
	}
}

void CUserInterface::UISetMIDIButtonChannel (unsigned uCh)
{
	// Mirrors the logic in Performance Config for handling MIDI channel configuration
	if (uCh == 0)
	{
		m_nMIDIButtonCh = CMIDIDevice::Disabled;
		LOGNOTE("MIDI Button channel not set");
	}
	else if (uCh <= CMIDIDevice::Channels)
	{
		m_nMIDIButtonCh = uCh - 1;
		LOGNOTE("MIDI Button channel set to: %d", m_nMIDIButtonCh+1);
	}
	else
	{
		m_nMIDIButtonCh = CMIDIDevice::OmniMode;
		LOGNOTE("MIDI Button channel set to: OMNI");
	}
}

// MCP23017 interrupt handler for Port A (INTA)
void CUserInterface::MCPInterruptHandlerA (void *pParam)
{
	g_bMCPInterruptA = true;
}

// MCP23017 interrupt handler for Port B (INTB)
void CUserInterface::MCPInterruptHandlerB (void *pParam)
{
	g_bMCPInterruptB = true;
}

// Process MCP23017 input changes (called from Process())
void CUserInterface::ProcessMCPInput (void)
{
	bool bReadA = g_bMCPInterruptA;
	bool bReadB = g_bMCPInterruptB;

	if (bReadA)
	{
		g_bMCPInterruptA = false;
		m_nMCPPortA = m_pMCP->ReadIntcapA ();
	}

	if (bReadB)
	{
		g_bMCPInterruptB = false;
		m_nMCPPortB = m_pMCP->ReadIntcapB ();
	}

	if (bReadA || bReadB)
	{
		// Get encoder pin states from configured pins
		unsigned nClockPin = m_pConfig->GetEncoderPinClock ();
		unsigned nDataPin = m_pConfig->GetEncoderPinData ();
		
		bool bEncA = false;
		bool bEncB = false;
		
		// Read encoder Clock pin
		if (IsMCPPin (nClockPin))
		{
			uint8_t nBit = 1 << MCPPinToBit (nClockPin);
			bEncA = ((IsMCPPortA (nClockPin) ? m_nMCPPortA : m_nMCPPortB) & nBit) != 0;
		}
		
		// Read encoder Data pin
		if (IsMCPPin (nDataPin))
		{
			uint8_t nBit = 1 << MCPPinToBit (nDataPin);
			bEncB = ((IsMCPPortA (nDataPin) ? m_nMCPPortA : m_nMCPPortB) & nBit) != 0;
		}
		
		DecodeMCPEncoder (bEncA, bEncB);

		// Process button edge detection
		ProcessMCPButtons (m_nMCPPortA, m_nMCPPortB);

		// Update last states
		m_nMCPLastPortA = m_nMCPPortA;
		m_nMCPLastPortB = m_nMCPPortB;
	}
}

// Decode MCP23017 encoder using Gray code state machine
void CUserInterface::DecodeMCPEncoder (bool bEncA, bool bEncB)
{
	// Gray code transition table for rotary encoder
	// Index = (lastAB << 2) | currentAB
	// Values: 0 = no change, 1 = CW step, -1 = CCW step, 2 = invalid
	static const int8_t s_EncoderTable[16] = {
		 0,  // 00 -> 00: no change
		-1,  // 00 -> 01: CCW
		 1,  // 00 -> 10: CW
		 2,  // 00 -> 11: invalid (skip)
		 1,  // 01 -> 00: CW
		 0,  // 01 -> 01: no change
		 2,  // 01 -> 10: invalid (skip)
		-1,  // 01 -> 11: CCW
		-1,  // 10 -> 00: CCW
		 2,  // 10 -> 01: invalid (skip)
		 0,  // 10 -> 10: no change
		 1,  // 10 -> 11: CW
		 2,  // 11 -> 00: invalid (skip)
		 1,  // 11 -> 01: CW
		-1,  // 11 -> 10: CCW
		 0   // 11 -> 11: no change
	};

	uint8_t nCurrentAB = (bEncA ? 1 : 0) | (bEncB ? 2 : 0);
	uint8_t nIndex = (m_nMCPLastAB << 2) | nCurrentAB;
	int8_t nDelta = s_EncoderTable[nIndex];

	m_nMCPLastAB = nCurrentAB;

	if (nDelta == 1)
	{
		// Clockwise step - fire StepUp event
		if (m_bSwitchPressed)
		{
			m_Menu.EventHandler (CUIMenu::MenuEventPressAndStepUp);
		}
		else
		{
			m_Menu.EventHandler (CUIMenu::MenuEventStepUp);
		}
	}
	else if (nDelta == -1)
	{
		// Counter-clockwise step - fire StepDown event
		if (m_bSwitchPressed)
		{
			m_Menu.EventHandler (CUIMenu::MenuEventPressAndStepDown);
		}
		else
		{
			m_Menu.EventHandler (CUIMenu::MenuEventStepDown);
		}
	}
	// nDelta == 0 or 2: no action (no change or invalid transition)
}

// Helper: Get MCP pin state for a given pin code
static bool GetMCPPinState (unsigned nPin, uint8_t nPortA, uint8_t nPortB, uint8_t nLastPortA, uint8_t nLastPortB, bool bCurrent)
{
	if (!IsMCPPin (nPin))
	{
		return false;  // Not an MCP pin
	}
	
	uint8_t nBit = 1 << MCPPinToBit (nPin);
	uint8_t nPort = IsMCPPortA (nPin) ? (bCurrent ? nPortA : nLastPortA) : (bCurrent ? nPortB : nLastPortB);
	
	// Active low: return true if pressed (bit is 0)
	return (nPort & nBit) == 0;
}

// Process MCP23017 button edge detection
void CUserInterface::ProcessMCPButtons (uint8_t nPortA, uint8_t nPortB)
{
	// Detect falling edges (button press, active low) using configured pins
	
	// Select button (also encoder switch)
	unsigned nSelectPin = m_pConfig->GetButtonPinSelect ();
	if (IsMCPPin (nSelectPin))
	{
		bool bNow = GetMCPPinState (nSelectPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, true);
		bool bWas = GetMCPPinState (nSelectPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, false);
		
		if (bNow && !bWas)
		{
			m_bSwitchPressed = true;
		}
		else if (!bNow && bWas)
		{
			m_bSwitchPressed = false;
			m_Menu.EventHandler (CUIMenu::MenuEventSelect);
		}
	}
	
	// Home button
	unsigned nHomePin = m_pConfig->GetButtonPinHome ();
	if (IsMCPPin (nHomePin) && nHomePin != nSelectPin)  // Avoid duplicate if same pin
	{
		bool bNow = GetMCPPinState (nHomePin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, true);
		bool bWas = GetMCPPinState (nHomePin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, false);
		
		if (bNow && !bWas)
		{
			m_Menu.EventHandler (CUIMenu::MenuEventHome);
		}
	}
	
	// Back button
	unsigned nBackPin = m_pConfig->GetButtonPinBack ();
	if (IsMCPPin (nBackPin) && nBackPin != nSelectPin)
	{
		bool bNow = GetMCPPinState (nBackPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, true);
		bool bWas = GetMCPPinState (nBackPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, false);
		
		if (bNow && !bWas)
		{
			m_Menu.EventHandler (CUIMenu::MenuEventBack);
		}
	}
	
	// Prev button
	unsigned nPrevPin = m_pConfig->GetButtonPinPrev ();
	if (IsMCPPin (nPrevPin))
	{
		bool bNow = GetMCPPinState (nPrevPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, true);
		bool bWas = GetMCPPinState (nPrevPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, false);
		
		if (bNow && !bWas)
		{
			m_Menu.EventHandler (CUIMenu::MenuEventStepDown);
		}
	}
	
	// Next button
	unsigned nNextPin = m_pConfig->GetButtonPinNext ();
	if (IsMCPPin (nNextPin))
	{
		bool bNow = GetMCPPinState (nNextPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, true);
		bool bWas = GetMCPPinState (nNextPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, false);
		
		if (bNow && !bWas)
		{
			m_Menu.EventHandler (CUIMenu::MenuEventStepUp);
		}
	}
}
