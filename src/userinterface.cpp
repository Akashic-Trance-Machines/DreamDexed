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

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>

#include <circle/font.h>
#include <circle/gpiomanager.h>
#include <circle/gpiopin.h>
#include <circle/i2cmaster.h>
#include <circle/logger.h>
#include <circle/spimaster.h>
#include <circle/startup.h>
#include <circle/string.h>
#include <circle/timer.h>
#include <circle/writebuffer.h>
#include <display/hd44780device.h>
#include <display/ssd1306device.h>
#include <display/ssd1309display.h>
#include <display/st7789device.h>
#include <display/st7789display.h>
#include <gpio/mcp23017.h>
#include <sensor/ky040.h>

#include "config.h"
#include "mididevice.h"
#include "minidexed.h"
#include "uibuttons.h"
#include "uimenu.h"

LOGMODULE("ui");

CUserInterface::CUserInterface(CMiniDexed *pMiniDexed, CGPIOManager *pGPIOManager, CI2CMaster *pI2CMaster, CSPIMaster *pSPIMaster, CConfig *pConfig) :
m_pMiniDexed{pMiniDexed},
m_pGPIOManager{pGPIOManager},
m_pI2CMaster{pI2CMaster},
m_pSPIMaster{pSPIMaster},
m_pConfig{pConfig},
m_pLCD{},
m_pHD44780{},
m_pSSD1306{},
m_pSSD1309{},
m_pST7789Display{},
m_pST7789{},
m_pLCDBuffered{},
m_pUIButtons{},
m_pRotaryEncoder{},
m_bSwitchPressed{},
m_pMCP{},
m_bUseMCP{false},
m_nLastPortA{0xFF},
m_nDebounceTimer{0},
m_Menu{this, pMiniDexed, pConfig}
{
	for (unsigned i = 0; i < NUM_ENCODERS; i++)
	{
		m_nLastEncoderState[i] = 0x03; // both channels HIGH (pull-up default)
		m_nEncoderAccumulator[i] = 0;
	}
}

CUserInterface::~CUserInterface()
{
	delete m_pRotaryEncoder;
	delete m_pUIButtons;
	delete m_pLCDBuffered;
	delete m_pLCD;
	delete m_pMCP;
	delete m_pSSD1309;
}

bool CUserInterface::Initialize()
{
	assert(m_pConfig);

	// Check if MCP23017 is the input device
	m_bUseMCP = (strcmp(m_pConfig->GetUIInputDevice(), "mcp23017") == 0);

	if (m_bUseMCP)
	{
		// Hardware reset MCP23017
		unsigned nMCPResetPin = m_pConfig->GetMCPResetGPIO();
		CGPIOPin mcpResetPin(nMCPResetPin, GPIOModeOutput);
		mcpResetPin.Write(HIGH);
		CTimer::SimpleMsDelay(10);
		mcpResetPin.Write(LOW);
		CTimer::SimpleMsDelay(10);
		mcpResetPin.Write(HIGH);
		CTimer::SimpleMsDelay(100);

		// Initialize MCP23017 I2C GPIO expander
		m_pMCP = new CMCP23017(m_pI2CMaster,
				       static_cast<u8>(m_pConfig->GetMCPAddress()));
		assert(m_pMCP);

		if (!m_pMCP->Initialize())
		{
			LOGERR("MCP23017 initialization failed");
			return false;
		}

		LOGDBG("MCP23017 initialized at 0x%02X", m_pConfig->GetMCPAddress());
	}

	// SSD1309 OLED hardware reset (Option B: use CSSD1309Display for reset only)
	unsigned nOLEDResetPin = m_pConfig->GetOLEDResetGPIO();
	if (nOLEDResetPin != 0)
	{
		// Create SSD1309Display just for hardware reset
		m_pSSD1309 = new CSSD1309Display(m_pI2CMaster,
						 nOLEDResetPin,
						 m_pConfig->GetSSD1306LCDI2CAddress());
		assert(m_pSSD1309);

		// Initialize performs hardware reset and SSD1309-specific init sequence
		if (!m_pSSD1309->Initialize())
		{
			LOGDBG("SSD1309 hardware reset/init failed, continuing with SSD1306 driver");
		}
		else
		{
			LOGDBG("SSD1309 hardware reset and init complete");
		}
	}

	if (m_pConfig->GetLCDEnabled())
	{
		uint8_t i2caddr = m_pConfig->GetLCDI2CAddress();
		uint8_t ssd1306addr = m_pConfig->GetSSD1306LCDI2CAddress();
		bool st7789 = m_pConfig->GetST7789Enabled();
		if (ssd1306addr != 0)
		{
			m_pSSD1306 = new CSSD1306Device(m_pConfig->GetSSD1306LCDWidth(), m_pConfig->GetSSD1306LCDHeight(),
							m_pI2CMaster, ssd1306addr,
							m_pConfig->GetSSD1306LCDRotate(), m_pConfig->GetSSD1306LCDMirror());
			if (!m_pSSD1306->Initialize())
			{
				LOGDBG("LCD: SSD1306 initialization failed");
				return false;
			}
			LOGDBG("LCD: SSD1306");
			m_pLCD = m_pSSD1306;
		}
		else if (st7789)
		{
			if (m_pSPIMaster == nullptr)
			{
				LOGDBG("LCD: ST7789 Enabled but SPI Initialisation Failed");
				return false;
			}

			unsigned nSPIClock = 1000 * m_pConfig->GetSPIClockKHz();
			unsigned nSPIMode = m_pConfig->GetSPIMode();
			unsigned nCPHA = (nSPIMode & 1) ? 1 : 0;
			unsigned nCPOL = (nSPIMode & 2) ? 1 : 0;
			LOGDBG("SPI: CPOL=%d; CPHA=%d; CLK=%d", nCPOL, nCPHA, nSPIClock);
			m_pST7789Display = new CST7789Display(m_pSPIMaster,
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

				m_pST7789Display->SetRotation(m_pConfig->GetST7789Rotation());

				bool bDoubleFont = m_pConfig->GetST7789FontSize() == 16 ? true : false;
				const TFont &font = m_pConfig->GetST7789FontSize() == 12 ? DDFont12x22 : DDFont8x16;

				m_pST7789 = new CST7789Device(m_pSPIMaster,
							      m_pST7789Display,
							      static_cast<unsigned>(m_pConfig->GetLCDColumns()),
							      static_cast<unsigned>(m_pConfig->GetLCDRows()),
							      font, bDoubleFont, bDoubleFont);
				if (m_pST7789->Initialize())
				{
					LOGDBG("LCD: ST7789");
					m_pLCD = m_pST7789;
				}
				else
				{
					LOGDBG("LCD: Failed to initalize ST7789 character device");
					delete (m_pST7789);
					delete (m_pST7789Display);
					m_pST7789 = nullptr;
					m_pST7789Display = nullptr;
					return false;
				}
			}
			else
			{
				LOGDBG("LCD: Failed to initialize ST7789 display");
				delete (m_pST7789Display);
				m_pST7789Display = nullptr;
				return false;
			}
		}
		else if (i2caddr == 0)
		{
			m_pHD44780 = new CHD44780Device(static_cast<unsigned>(m_pConfig->GetLCDColumns()),
							static_cast<unsigned>(m_pConfig->GetLCDRows()),
							m_pConfig->GetLCDPinData4(),
							m_pConfig->GetLCDPinData5(),
							m_pConfig->GetLCDPinData6(),
							m_pConfig->GetLCDPinData7(),
							m_pConfig->GetLCDPinEnable(),
							m_pConfig->GetLCDPinRegisterSelect(),
							m_pConfig->GetLCDPinReadWrite());
			if (!m_pHD44780->Initialize())
			{
				LOGDBG("LCD: HD44780 initialization failed");
				return false;
			}
			LOGDBG("LCD: HD44780");
			m_pLCD = m_pHD44780;
		}
		else
		{
			m_pHD44780 = new CHD44780Device(m_pI2CMaster, i2caddr,
							static_cast<unsigned>(m_pConfig->GetLCDColumns()),
							static_cast<unsigned>(m_pConfig->GetLCDRows()));
			if (!m_pHD44780->Initialize())
			{
				LOGDBG("LCD: HD44780 (I2C) initialization failed");
				return false;
			}
			LOGDBG("LCD: HD44780 I2C");
			m_pLCD = m_pHD44780;
		}
		assert(m_pLCD);

		m_pLCDBuffered = new CWriteBufferDevice(m_pLCD);
		assert(m_pLCDBuffered);
		// clear sceen and go to top left corner
		LCDWrite("\x1B[H\x1B[J"); // cursor home and clear screen
		LCDWrite("\x1B[?25l\x1B"
			 "d+"); // cursor off, autopage mode
		LCDWrite("MiniDexed\nLoading...");
		m_pLCDBuffered->Update();

		LOGDBG("LCD initialized");
	}

	m_pUIButtons = new CUIButtons(m_pConfig);
	assert(m_pUIButtons);

	if (!m_pUIButtons->Initialize())
	{
		return false;
	}

	m_pUIButtons->RegisterEventHandler(UIButtonsEventStub, this);
	UISetMIDIButtonChannel(m_pConfig->GetMIDIButtonCh());

	LOGDBG("Button User Interface initialized");

	if (m_pConfig->GetEncoderEnabled() && !m_bUseMCP)
	{
		m_pRotaryEncoder = new CKY040(m_pConfig->GetEncoderPinClock(),
					      m_pConfig->GetEncoderPinData(),
					      m_pConfig->GetButtonPinShortcut(),
					      m_pGPIOManager,
					      m_pConfig->GetEncoderDetents());
		assert(m_pRotaryEncoder);

		if (!m_pRotaryEncoder->Initialize())
		{
			return false;
		}

		m_pRotaryEncoder->RegisterEventHandler(EncoderEventStub, this);

		LOGDBG("Rotary encoder initialized");
	}

	m_Menu.EventHandler(CUIMenu::MenuEventUpdate);

	return true;
}

void CUserInterface::LoadDefaultScreen()
{
	// performance load
	if (m_pConfig->GetDefaultScreen() == 1)
	{
		m_Menu.EventHandler(CUIMenu::MenuEventStepDown);
		m_Menu.EventHandler(CUIMenu::MenuEventSelect);
		m_Menu.EventHandler(CUIMenu::MenuEventSelect);
	}
}

void CUserInterface::Process()
{
	if (m_pLCDBuffered)
	{
		m_pLCDBuffered->Update();
	}
	if (m_pUIButtons)
	{
		m_pUIButtons->Update();
	}
	if (m_bUseMCP && m_pMCP)
	{
		PollMCP();
	}
}

void CUserInterface::PollMCP()
{
	assert(m_pMCP);

	// Read both MCP23017 ports
	uint8_t nPortA = m_pMCP->ReadPortA(); // buttons + encoder clicks
	uint8_t nPortB = m_pMCP->ReadPortB(); // encoder rotation

	unsigned nPulsePerStep = m_pConfig->GetEncoderPulsePerStep();
	if (nPulsePerStep == 0)
	{
		nPulsePerStep = 1;
	}

	// --- Decode 4 encoders from Port B ---
	// Per GPIO_PINOUT.md, channels are swapped (B read as A, A read as B):
	// Encoder 1 (top):    GPB0=ChB, GPB1=ChA
	// Encoder 2:          GPB2=ChB, GPB3=ChA
	// Encoder 3:          GPB4=ChB, GPB5=ChA
	// Encoder 4 (bottom): GPB6=ChB, GPB7=ChA
	for (unsigned enc = 0; enc < NUM_ENCODERS; enc++)
	{
		// Extract the 2 bits for this encoder (swapped: bit0=B, bit1=A)
		unsigned nBitOffset = enc * 2;
		uint8_t nRawB = (nPortB >> nBitOffset) & 0x01;     // Channel B
		uint8_t nRawA = (nPortB >> (nBitOffset + 1)) & 0x01; // Channel A

		// Current state: A in bit1, B in bit0
		uint8_t nCurrentState = (nRawA << 1) | nRawB;
		uint8_t nPrevState = m_nLastEncoderState[enc];

		if (nCurrentState != nPrevState)
		{
			// Quadrature state table for direction detection
			// State transitions: 00->01->11->10 = CW, 00->10->11->01 = CCW
			static const int8_t quadTable[16] = {
				 0, -1,  1,  0,
				 1,  0,  0, -1,
				-1,  0,  0,  1,
				 0,  1, -1,  0
			};

			int8_t nDirection = quadTable[(nPrevState << 2) | nCurrentState];
			m_nEncoderAccumulator[enc] += nDirection;

			// Only emit event when accumulator reaches threshold
			if (m_nEncoderAccumulator[enc] >= (int)nPulsePerStep)
			{
				m_nEncoderAccumulator[enc] = 0;
				// Encoder 0 is the main encoder for menu navigation
				m_Menu.EventHandler(CUIMenu::MenuEventStepUp);
			}
			else if (m_nEncoderAccumulator[enc] <= -(int)nPulsePerStep)
			{
				m_nEncoderAccumulator[enc] = 0;
				m_Menu.EventHandler(CUIMenu::MenuEventStepDown);
			}

			m_nLastEncoderState[enc] = nCurrentState;
		}
	}

	// --- Decode Port A buttons (active-low, active = 0) ---
	// Only process on state change (simple debounce)
	if (nPortA != m_nLastPortA)
	{
		// Detect falling edges (button press: was HIGH, now LOW)
		uint8_t nPressed = m_nLastPortA & ~nPortA;

		// Encoder clicks (GPA0-GPA3, reverse order per pinout)
		// GPA3 = Encoder 1 click (top), GPA0 = Encoder 4 click (bottom)
		if (nPressed & (1 << 3)) // GPA3 = Encoder 1 click
		{
			m_Menu.EventHandler(CUIMenu::MenuEventSelect);
		}
		if (nPressed & (1 << 2)) // GPA2 = Encoder 2 click
		{
			m_Menu.EventHandler(CUIMenu::MenuEventSelect);
		}
		if (nPressed & (1 << 1)) // GPA1 = Encoder 3 click
		{
			m_Menu.EventHandler(CUIMenu::MenuEventSelect);
		}
		if (nPressed & (1 << 0)) // GPA0 = Encoder 4 click
		{
			m_Menu.EventHandler(CUIMenu::MenuEventSelect);
		}

		// Nav buttons (GPA4-GPA7, reverse order per pinout)
		// GPA7 = Main (top), GPA6 = Voice, GPA5 = FX, GPA4 = Mix (bottom)
		if (nPressed & (1 << 7)) // GPA7 = Main page
		{
			m_Menu.EventHandler(CUIMenu::MenuEventHome);
		}
		if (nPressed & (1 << 6)) // GPA6 = Voice page
		{
			m_Menu.EventHandler(CUIMenu::MenuEventBack);
		}
		if (nPressed & (1 << 5)) // GPA5 = FX page
		{
			m_Menu.EventHandler(CUIMenu::MenuEventPgmUp);
		}
		if (nPressed & (1 << 4)) // GPA4 = Mix page
		{
			m_Menu.EventHandler(CUIMenu::MenuEventPgmDown);
		}

		m_nLastPortA = nPortA;
	}
}

void CUserInterface::ParameterChanged()
{
	m_Menu.EventHandler(CUIMenu::MenuEventUpdateParameter);
}

void CUserInterface::DisplayChanged()
{
	m_Menu.EventHandler(CUIMenu::MenuEventUpdate);
}

void CUserInterface::DisplayWrite(const char *pMenu, const char *pParam, const char *pValue,
				  bool bArrowDown, bool bArrowUp)
{
	assert(pMenu);
	assert(pParam);
	assert(pValue);

	size_t nLineMaxLen = static_cast<size_t>(m_pConfig->GetLCDColumns());

	const char *pHdr = "\x1B[H\E[?25l"; // cursor home and off
	size_t nHdrLen = strlen(pHdr);

	const char *pClear = "\x1B[K"; // clear end of line
	size_t nClearLen = strlen(pClear);

	size_t nParamLen = std::min(nLineMaxLen, strlen(pParam));
	size_t nMenuLen = strlen(pMenu);
	size_t nFill1Len = nLineMaxLen > nParamLen + nMenuLen ? nLineMaxLen - nParamLen - nMenuLen : 1;

	nFill1Len = std::min(nLineMaxLen - nParamLen, nFill1Len);
	nMenuLen = std::min(nLineMaxLen - nParamLen - nFill1Len, nMenuLen);

	size_t nLine1Len = nParamLen + nFill1Len + nMenuLen;

	size_t nArrowsLen = 2;
	size_t nValueLen = std::min(nLineMaxLen - nArrowsLen, strlen(pValue));
	size_t nFill2Len = bArrowUp ? nLineMaxLen - nArrowsLen - nValueLen : 0;
	size_t nLine2Len = nValueLen + nFill2Len + nArrowsLen;

	if (nLine2Len >= nLineMaxLen)
		nClearLen = 0;

	size_t nOffset = 0;

	char pLines[nHdrLen + nLine1Len + nLine2Len + nClearLen + 1];

	memcpy(pLines, pHdr, nHdrLen);
	nOffset += nHdrLen;

	memcpy(pLines + nOffset, pParam, nParamLen);
	nOffset += nParamLen;

	memset(pLines + nOffset, ' ', nFill1Len);
	nOffset += nFill1Len;

	memcpy(pLines + nOffset, pMenu, nMenuLen);
	nOffset += nMenuLen;

	pLines[nOffset++] = bArrowDown ? '<' : ' ';

	memcpy(pLines + nOffset, pValue, nValueLen);
	nOffset += nValueLen;

	memset(pLines + nOffset, ' ', nFill2Len);
	nOffset += nFill2Len;

	pLines[nOffset++] = bArrowUp ? '>' : ' ';

	memcpy(pLines + nOffset, pClear, nClearLen);
	nOffset += nClearLen;

	pLines[nOffset++] = 0;

	LCDWrite(pLines);
}

void CUserInterface::LCDWrite(const char *pString)
{
	if (m_pLCDBuffered)
	{
		m_pLCDBuffered->Write(pString, strlen(pString));
	}
}

void CUserInterface::EncoderEventHandler(CKY040::TEvent Event)
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
		if (m_bSwitchPressed)
		{
			// We must reset the encoder switch button to prevent events from being
			// triggered after the encoder is rotated
			m_pUIButtons->ResetButton(m_pConfig->GetButtonPinShortcut());
			m_Menu.EventHandler(CUIMenu::MenuEventPressAndStepUp);
		}
		else
		{
			m_Menu.EventHandler(CUIMenu::MenuEventStepUp);
		}
		break;

	case CKY040::EventCounterclockwise:
		if (m_bSwitchPressed)
		{
			m_pUIButtons->ResetButton(m_pConfig->GetButtonPinShortcut());
			m_Menu.EventHandler(CUIMenu::MenuEventPressAndStepDown);
		}
		else
		{
			m_Menu.EventHandler(CUIMenu::MenuEventStepDown);
		}
		break;

	case CKY040::EventSwitchHold:
		if (m_pRotaryEncoder->GetHoldSeconds() >= 120)
		{
			delete m_pLCD; // reset LCD

			reboot();
		}
		break;

	default:
		break;
	}
}

void CUserInterface::EncoderEventStub(CKY040::TEvent Event, void *pParam)
{
	CUserInterface *pThis = static_cast<CUserInterface *>(pParam);
	assert(pThis != 0);

	pThis->EncoderEventHandler(Event);
}

void CUserInterface::UIButtonsEventHandler(CUIButton::BtnEvent Event)
{
	switch (Event)
	{
	case CUIButton::BtnEventPrev:
		m_Menu.EventHandler(CUIMenu::MenuEventStepDown);
		break;

	case CUIButton::BtnEventNext:
		m_Menu.EventHandler(CUIMenu::MenuEventStepUp);
		break;

	case CUIButton::BtnEventBack:
		m_Menu.EventHandler(CUIMenu::MenuEventBack);
		break;

	case CUIButton::BtnEventSelect:
		m_Menu.EventHandler(CUIMenu::MenuEventSelect);
		break;

	case CUIButton::BtnEventHome:
		m_Menu.EventHandler(CUIMenu::MenuEventHome);
		break;

	case CUIButton::BtnEventPgmUp:
		m_Menu.EventHandler(CUIMenu::MenuEventPgmUp);
		break;

	case CUIButton::BtnEventPgmDown:
		m_Menu.EventHandler(CUIMenu::MenuEventPgmDown);
		break;

	case CUIButton::BtnEventBankUp:
		m_Menu.EventHandler(CUIMenu::MenuEventBankUp);
		break;

	case CUIButton::BtnEventBankDown:
		m_Menu.EventHandler(CUIMenu::MenuEventBankDown);
		break;

	case CUIButton::BtnEventTGUp:
		m_Menu.EventHandler(CUIMenu::MenuEventTGUp);
		break;

	case CUIButton::BtnEventTGDown:
		m_Menu.EventHandler(CUIMenu::MenuEventTGDown);
		break;

	default:
		break;
	}
}

void CUserInterface::UIButtonsEventStub(CUIButton::BtnEvent Event, void *pParam)
{
	CUserInterface *pThis = static_cast<CUserInterface *>(pParam);
	assert(pThis != 0);

	pThis->UIButtonsEventHandler(Event);
}

void CUserInterface::UIMIDICmdHandler(int nMidiCh, uint8_t nMidiType, uint8_t nMidiData1, uint8_t nMidiData2)
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
		m_pUIButtons->BtnMIDICmdHandler(nMidiType, nMidiData1, nMidiData2);
	}
}

void CUserInterface::UISetMIDIButtonChannel(int nCh)
{
	// Mirrors the logic in Performance Config for handling MIDI channel configuration
	if (nCh == 0)
	{
		m_nMIDIButtonCh = CMIDIDevice::Disabled;
		LOGNOTE("MIDI Button channel not set");
	}
	else if (nCh <= CMIDIDevice::Channels)
	{
		m_nMIDIButtonCh = nCh - 1;
		LOGNOTE("MIDI Button channel set to: %d", m_nMIDIButtonCh + 1);
	}
	else
	{
		m_nMIDIButtonCh = CMIDIDevice::OmniMode;
		LOGNOTE("MIDI Button channel set to: OMNI");
	}
}
