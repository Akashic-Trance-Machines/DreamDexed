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
	m_nMCPButtonPressTime (0),
	m_nMCPButtonClickTime (0),
	m_nMCPButtonClicks (0),
	m_bMCPButtonHeld (false),
	m_nMCPEncoderSteps (0),
	m_nLastWaveformUpdate (0),
	m_pSSD1306Gfx (0),
	m_Menu (this, pMiniDexed, pConfig)
{
	memset (m_WaveformSnapshot, 0, sizeof (m_WaveformSnapshot));
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

			// Create waveform/status graphics overlay if display is 32 or 64 pixels tall
			// and either waveform or MIDI channel display is enabled
			if (m_pConfig->GetSSD1306LCDHeight () >= 32 && 
			    (m_pConfig->GetLCDShowWaveform () || m_pConfig->GetLCDShowMidiChannel ()))
			{
				m_pSSD1306Gfx = new CSSD1306Gfx (m_pI2CMaster, ssd1306addr, m_pConfig->GetSSD1306LCDHeight ());
				LOGDBG ("LCD: Graphics overlay enabled");
			}
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
		LOGDBG ("MCP23017 init: address=0x%02X, INTA=GPIO%u, INTB=GPIO%u",
		        m_pConfig->GetMCPAddress (),
		        m_pConfig->GetMCPAInterruptGPIO (),
		        m_pConfig->GetMCPBInterruptGPIO ());

		m_pMCP = new CMCP23017 (*m_pI2CMaster, m_pConfig->GetMCPAddress ());
		assert (m_pMCP);

		// Initialize Port A and B for UI inputs (all 8 bits each)
		if (!m_pMCP->Init_UI_PortA (0xFF))
		{
			LOGERR ("MCP23017 Port A initialization failed");
			return false;
		}
		LOGDBG ("MCP23017 Port A initialized (inputs with pull-ups, interrupts enabled)");

		if (!m_pMCP->Init_UI_PortB (0xFF))
		{
			LOGERR ("MCP23017 Port B initialization failed");
			return false;
		}
		LOGDBG ("MCP23017 Port B initialized (inputs with pull-ups, interrupts enabled)");

		// Setup GPIO interrupt pin for Port A
		// NOTE: MCP23017 INT is active-low and STAYS low until INTCAP/GPIO read
		// Use LOW LEVEL detection (not falling edge) so interrupt keeps firing until cleared
		unsigned nIntPinA = m_pConfig->GetMCPAInterruptGPIO ();
		if (nIntPinA > 0)
		{
			m_pMCPInterruptPinA = new CGPIOPin (nIntPinA, GPIOModeInputPullUp, m_pGPIOManager);
			m_pMCPInterruptPinA->ConnectInterrupt (MCPInterruptHandlerA, this);
			m_pMCPInterruptPinA->EnableInterrupt (GPIOInterruptOnLowLevel);
			LOGDBG ("MCP23017 INTA on GPIO%u (low level)", nIntPinA);
		}

		// Setup GPIO interrupt pin for Port B
		unsigned nIntPinB = m_pConfig->GetMCPBInterruptGPIO ();
		if (nIntPinB > 0)
		{
			m_pMCPInterruptPinB = new CGPIOPin (nIntPinB, GPIOModeInputPullUp, m_pGPIOManager);
			m_pMCPInterruptPinB->ConnectInterrupt (MCPInterruptHandlerB, this);
			m_pMCPInterruptPinB->EnableInterrupt (GPIOInterruptOnLowLevel);
			LOGDBG ("MCP23017 INTB on GPIO%u (low level)", nIntPinB);
		}

		// Read initial port states and log them
		m_nMCPPortA = m_pMCP->ReadGpioA ();
		m_nMCPPortB = m_pMCP->ReadGpioB ();
		m_nMCPLastPortA = m_nMCPPortA;
		m_nMCPLastPortB = m_nMCPPortB;

		LOGDBG ("MCP23017 initial state: PortA=0x%02X, PortB=0x%02X", m_nMCPPortA, m_nMCPPortB);
		LOGDBG ("MCP23017 initialized successfully");
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

	// Render waveform if enabled
	if (m_pConfig->GetLCDShowWaveform ())
	{
		RenderWaveform ();
	}

	if (m_pConfig->GetLCDShowMidiChannel ())
	{
		RenderMidiStatus ();
	}
}

void CUserInterface::RenderMidiStatus (void)
{
	// Implementation for rendering MIDI status
	// This method was added based on the user's request to call it in Process()
	// The actual rendering logic is not provided in the instruction, so it's left empty.
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

	// POLLING FALLBACK: If no interrupt received, poll GPIO directly
	// This works around interrupt issues and helps debugging
	if (!bReadA && !bReadB)
	{
		// Poll MCP directly every time Process() is called
		uint8_t nNewPortA = m_pMCP->ReadGpioA ();
		uint8_t nNewPortB = m_pMCP->ReadGpioB ();
		
		if (nNewPortA != m_nMCPPortA)
		{
			LOGDBG ("MCP POLL: PortA=0x%02X (was 0x%02X)", nNewPortA, m_nMCPPortA);
			m_nMCPPortA = nNewPortA;
			bReadA = true;
		}
		if (nNewPortB != m_nMCPPortB)
		{
			LOGDBG ("MCP POLL: PortB=0x%02X (was 0x%02X)", nNewPortB, m_nMCPPortB);
			m_nMCPPortB = nNewPortB;
			bReadB = true;
		}
	}
	else
	{
		// Interrupt triggered - read INTCAP registers
		if (bReadA)
		{
			g_bMCPInterruptA = false;
			m_nMCPPortA = m_pMCP->ReadIntcapA ();
			LOGDBG ("MCP INTA: PortA=0x%02X (was 0x%02X)", m_nMCPPortA, m_nMCPLastPortA);
		}

		if (bReadB)
		{
			g_bMCPInterruptB = false;
			m_nMCPPortB = m_pMCP->ReadIntcapB ();
			LOGDBG ("MCP INTB: PortB=0x%02X (was 0x%02X)", m_nMCPPortB, m_nMCPLastPortB);
		}
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
		
		LOGDBG ("MCP Encoder: CLK=%d DATA=%d (pins %d,%d)", bEncA, bEncB, nClockPin, nDataPin);
		DecodeMCPEncoder (bEncA, bEncB);

		// Process button edge detection on port change
		ProcessMCPButtons (m_nMCPPortA, m_nMCPPortB, true);

		// Update last states
		m_nMCPLastPortA = m_nMCPPortA;
		m_nMCPLastPortB = m_nMCPPortB;
	}
	else
	{
		// No port change - still need to process button timers
		ProcessMCPButtons (m_nMCPPortA, m_nMCPPortB, false);
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

	if (nCurrentAB != m_nMCPLastAB)
	{
		LOGDBG ("MCP Encoder: AB %d->%d delta=%d", m_nMCPLastAB, nCurrentAB, nDelta);
	}
	m_nMCPLastAB = nCurrentAB;

	// Accumulate steps and fire events only when threshold is reached
	// This handles encoders that produce multiple pulses per detent
	if (nDelta == 1 || nDelta == -1)
	{
		m_nMCPEncoderSteps += nDelta;
		
		int nPulsePerStep = (int) m_pConfig->GetEncoderPulsePerStep ();
		
		while (m_nMCPEncoderSteps >= nPulsePerStep)
		{
			// Clockwise step - fire StepUp event
			LOGDBG ("MCP Encoder: CW step -> StepUp");
			if (m_bSwitchPressed)
			{
				m_Menu.EventHandler (CUIMenu::MenuEventPressAndStepUp);
			}
			else
			{
				m_Menu.EventHandler (CUIMenu::MenuEventStepUp);
			}
			m_nMCPEncoderSteps -= nPulsePerStep;
		}
		
		while (m_nMCPEncoderSteps <= -nPulsePerStep)
		{
			// Counter-clockwise step - fire StepDown event
			LOGDBG ("MCP Encoder: CCW step -> StepDown");
			if (m_bSwitchPressed)
			{
				m_Menu.EventHandler (CUIMenu::MenuEventPressAndStepDown);
			}
			else
			{
				m_Menu.EventHandler (CUIMenu::MenuEventStepDown);
			}
			m_nMCPEncoderSteps += nPulsePerStep;
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

// Process MCP23017 button edge detection with timing for click/doubleclick/longpress
void CUserInterface::ProcessMCPButtons (uint8_t nPortA, uint8_t nPortB, bool bPortChanged)
{
	// Timing constants (in milliseconds)
	const unsigned DOUBLECLICK_TIMEOUT = m_pConfig->GetDoubleClickTimeout ();
	const unsigned LONGPRESS_TIMEOUT = m_pConfig->GetLongPressTimeout ();
	
	unsigned nTicks = CTimer::GetClockTicks () / (CLOCKHZ / 1000);  // Current time in ms
	
	// Select button (also encoder switch) - supports click/doubleclick/longpress
	unsigned nSelectPin = m_pConfig->GetButtonPinSelect ();
	if (IsMCPPin (nSelectPin))
	{
		// Edge detection only happens when port actually changed
		if (bPortChanged)
		{
			bool bNow = GetMCPPinState (nSelectPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, true);
			bool bWas = GetMCPPinState (nSelectPin, nPortA, nPortB, m_nMCPLastPortA, m_nMCPLastPortB, false);
			
			if (bNow && !bWas)
			{
				// Button just pressed
				LOGDBG ("MCP Button: PRESSED (pin %d)", nSelectPin);
				m_bSwitchPressed = true;
				m_bMCPButtonHeld = true;
				m_nMCPButtonPressTime = nTicks;
			}
			else if (!bNow && bWas)
			{
				// Button just released
				m_bSwitchPressed = false;
				m_bMCPButtonHeld = false;
				unsigned nHoldTime = nTicks - m_nMCPButtonPressTime;
				
				if (nHoldTime >= LONGPRESS_TIMEOUT)
				{
					// Long press - event already fired while held
					LOGDBG ("MCP Button: RELEASED after long press");
				}
				else
				{
					// Short press - count as click
					unsigned nTimeSinceLastClick = nTicks - m_nMCPButtonClickTime;
					m_nMCPButtonClickTime = nTicks;
					
					if (nTimeSinceLastClick <= DOUBLECLICK_TIMEOUT && m_nMCPButtonClicks > 0)
					{
						// Double click!
						LOGDBG ("MCP Button: DOUBLE CLICK -> MenuEventHome");
						m_Menu.EventHandler (CUIMenu::MenuEventHome);
						m_nMCPButtonClicks = 0;
					}
					else
					{
						// First click - wait for possible second click
						m_nMCPButtonClicks = 1;
						LOGDBG ("MCP Button: CLICK (waiting for double click...)");
					}
				}
			}
		}
		
		// Long press check runs always (even without port change)
		if (m_bMCPButtonHeld)
		{
			unsigned nHoldTime = nTicks - m_nMCPButtonPressTime;
			if (nHoldTime >= LONGPRESS_TIMEOUT && m_nMCPButtonClicks == 0)
			{
				// Long press detected!
				LOGDBG ("MCP Button: LONG PRESS -> MenuEventBack");
				m_Menu.EventHandler (CUIMenu::MenuEventBack);
				m_nMCPButtonClicks = 99;  // Prevent repeat
			}
		}
	}
	
	// Check for single click timeout (fire click if no second click came) - runs always
	if (m_nMCPButtonClicks == 1 && !m_bMCPButtonHeld)
	{
		unsigned nTimeSinceClick = nTicks - m_nMCPButtonClickTime;
		if (nTimeSinceClick > DOUBLECLICK_TIMEOUT)
		{
			LOGDBG ("MCP Button: SINGLE CLICK -> MenuEventSelect");
			m_Menu.EventHandler (CUIMenu::MenuEventSelect);
			m_nMCPButtonClicks = 0;
		}
	}
	// Prev/Next buttons only need edge detection when port changes
	if (bPortChanged)
	{
		// Prev button (simple press detection)
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
		
		// Next button (simple press detection)
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
}

// Render audio waveform using pixel-level graphics on bottom half of SSD1306
// Uses CSSD1306Gfx overlay for direct framebuffer access (y=32-63)
void CUserInterface::RenderWaveform (void)
{
	// Only render if waveform graphics overlay is available and waveform enabled
	if (!m_pSSD1306Gfx || !m_pConfig->GetLCDShowWaveform())
	{
		return;
	}

	// Rate limit to ~20 FPS (50ms interval)
	unsigned nNow = CTimer::GetClockTicks () / (CLOCKHZ / 1000);
	if (nNow - m_nLastWaveformUpdate < 50)
	{
		return;
	}
	m_nLastWaveformUpdate = nNow;

	// Get snapshot from ring buffer
	m_pMiniDexed->GetWaveformBuffer ()->GetSnapshot (m_WaveformSnapshot);

	// Clear waveform area
	m_pSSD1306Gfx->Clear ();

	// Draw waveform: 128 samples -> 128 pixels
	// Center at y=16 (relative to our 32-pixel waveform area), amplitude ±15 pixels
	for (unsigned x = 0; x < 128; x++)
	{
		int sample = m_WaveformSnapshot[x];
		// Map sample [-127..127] to y [1..31] (center at 16)
		int y = 16 - (sample * 15 / 127);
		if (y < 0) y = 0;
		if (y > 31) y = 31;
		
		// Draw single pixel (oscilloscope style)
		m_pSSD1306Gfx->SetPixel (x, y);
	}

	// Send to display
	m_pSSD1306Gfx->UpdateDisplay ();
}

void CUserInterface::RenderMidiStatus (void)
{
	if (!m_pSSD1306Gfx)
	{
		return;
	}

	// Gather active note counts
	unsigned activeNotes[8];
	for (unsigned i = 0; i < 8; i++)
	{
		activeNotes[i] = m_pMiniDexed->GetActiveNotes(i);
	}

	m_pSSD1306Gfx->DrawMidiStatus(activeNotes);
}

