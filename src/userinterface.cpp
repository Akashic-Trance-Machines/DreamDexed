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
m_nMCPButtonCount{0},
m_nMCPEncoderCount{0},
m_nLastPortA{0xFF},
m_nLastPortB{0xFF},
m_Menu{this, pMiniDexed, pConfig},
m_pUI4Row{nullptr},
m_bUse4RowUI{false},
m_nMCPEncoderClickCount{0},
m_bNavEncoderMode{false},
m_nNavEncoderIndex{0}
{
	memset(m_MCPButtons, 0, sizeof(m_MCPButtons));
	memset(m_MCPEncoders, 0, sizeof(m_MCPEncoders));
	memset(m_MCPEncoderClicks, 0, sizeof(m_MCPEncoderClicks));
}

CUserInterface::~CUserInterface()
{
	delete m_pRotaryEncoder;
	delete m_pUIButtons;
	delete m_pLCDBuffered;
	delete m_pLCD;
	delete m_pMCP;
	delete m_pSSD1309;
	delete m_pUI4Row;
}

// Parse MCP pin string like "GPA0", "GPB7" → {valid, isPortA, bit}
TMCPPin CUserInterface::ParseMCPPin(const char *pStr)
{
	TMCPPin pin = {false, false, 0};

	if (!pStr || strlen(pStr) < 4)
		return pin;

	// Must start with "GP" or "gp"
	if ((pStr[0] != 'G' && pStr[0] != 'g') ||
	    (pStr[1] != 'P' && pStr[1] != 'p'))
		return pin;

	// Port letter: A or B
	if (pStr[2] == 'A' || pStr[2] == 'a')
		pin.bIsPortA = true;
	else if (pStr[2] == 'B' || pStr[2] == 'b')
		pin.bIsPortA = false;
	else
		return pin;

	// Bit number: 0-7
	if (pStr[3] < '0' || pStr[3] > '7')
		return pin;

	pin.nBit = pStr[3] - '0';
	pin.bValid = true;
	return pin;
}

// Add a button binding from config property to menu event
void CUserInterface::AddMCPButtonBinding(const char *pPinStr, CUIMenu::TMenuEvent Event)
{
	TMCPPin pin = ParseMCPPin(pPinStr);
	if (!pin.bValid)
		return;

	if (m_nMCPButtonCount >= MAX_MCP_BUTTONS)
		return;

	m_MCPButtons[m_nMCPButtonCount].bIsPortA = pin.bIsPortA;
	m_MCPButtons[m_nMCPButtonCount].nBit = pin.nBit;
	m_MCPButtons[m_nMCPButtonCount].Event = Event;
	m_nMCPButtonCount++;

	LOGDBG("MCP button: %s → event %d (port %c, bit %u)",
	       pPinStr, Event, pin.bIsPortA ? 'A' : 'B', pin.nBit);
}

bool CUserInterface::Initialize()
{
	assert(m_pConfig);

	// Check if MCP23017 is the input device
	m_bUseMCP = (strcmp(m_pConfig->GetUIInputDevice(), "mcp23017") == 0);

	// Check if 4-Row UI mode is selected
	m_bUse4RowUI = m_pConfig->Is4RowUI();

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

		// --- Parse button bindings from config ---
		// Navigation buttons
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinPrev", "0"),
				    CUIMenu::MenuEventStepDown);
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinNext", "0"),
				    CUIMenu::MenuEventStepUp);
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinBack", "0"),
				    CUIMenu::MenuEventBack);
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinSelect", "0"),
				    CUIMenu::MenuEventSelect);
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinHome", "0"),
				    CUIMenu::MenuEventHome);

		// Program/Bank/TG buttons
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinPgmUp", "0"),
				    CUIMenu::MenuEventPgmUp);
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinPgmDown", "0"),
				    CUIMenu::MenuEventPgmDown);
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinBankUp", "0"),
				    CUIMenu::MenuEventBankUp);
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinBankDown", "0"),
				    CUIMenu::MenuEventBankDown);
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinTGUp", "0"),
				    CUIMenu::MenuEventTGUp);
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinTGDown", "0"),
				    CUIMenu::MenuEventTGDown);

		// Encoder shortcut/click button
		AddMCPButtonBinding(m_pConfig->GetPropertyString("ButtonPinShortcut", "0"),
				    CUIMenu::MenuEventSelect);

		LOGDBG("MCP buttons configured: %u bindings", m_nMCPButtonCount);

		// --- Parse encoder bindings from config ---
		// In 4-row mode, skip the classic encoder — register 4 separate encoders below
		if (m_pConfig->GetEncoderEnabled() && !m_bUse4RowUI)
		{
			const char *pClockStr = m_pConfig->GetPropertyString("EncoderPinClock", "0");
			const char *pDataStr = m_pConfig->GetPropertyString("EncoderPinData", "0");

			TMCPPin clockPin = ParseMCPPin(pClockStr);
			TMCPPin dataPin = ParseMCPPin(pDataStr);

			if (clockPin.bValid && dataPin.bValid && m_nMCPEncoderCount < MAX_MCP_ENCODERS)
			{
				m_MCPEncoders[m_nMCPEncoderCount].bClockIsPortA = clockPin.bIsPortA;
				m_MCPEncoders[m_nMCPEncoderCount].nClockBit = clockPin.nBit;
				m_MCPEncoders[m_nMCPEncoderCount].bDataIsPortA = dataPin.bIsPortA;
				m_MCPEncoders[m_nMCPEncoderCount].nDataBit = dataPin.nBit;
				m_MCPEncoders[m_nMCPEncoderCount].nLastState = 0x03; // both HIGH (pull-up)
				m_MCPEncoders[m_nMCPEncoderCount].nAccumulator = 0;
				m_nMCPEncoderCount++;

				LOGDBG("MCP encoder: clock=%s data=%s", pClockStr, pDataStr);
			}
		}

		LOGDBG("MCP encoders configured: %u bindings", m_nMCPEncoderCount);

		// If 4-row mode, set up dedicated encoder and button bindings
		if (m_bUse4RowUI)
		{
			// 4-Row encoder rotation bindings (4 encoders)
			const char *encPinNames[][2] = {
				{"4RowEnc1ClkPin", "4RowEnc1DataPin"},
				{"4RowEnc2ClkPin", "4RowEnc2DataPin"},
				{"4RowEnc3ClkPin", "4RowEnc3DataPin"},
				{"4RowEnc4ClkPin", "4RowEnc4DataPin"},
			};

			for (unsigned i = 0; i < 4; i++)
			{
				const char *pClk = m_pConfig->GetPropertyString(encPinNames[i][0], "0");
				const char *pDat = m_pConfig->GetPropertyString(encPinNames[i][1], "0");

				TMCPPin clockPin = ParseMCPPin(pClk);
				TMCPPin dataPin = ParseMCPPin(pDat);

				if (clockPin.bValid && dataPin.bValid && m_nMCPEncoderCount < MAX_MCP_ENCODERS)
				{
					m_MCPEncoders[m_nMCPEncoderCount].bClockIsPortA = clockPin.bIsPortA;
					m_MCPEncoders[m_nMCPEncoderCount].nClockBit = clockPin.nBit;
					m_MCPEncoders[m_nMCPEncoderCount].bDataIsPortA = dataPin.bIsPortA;
					m_MCPEncoders[m_nMCPEncoderCount].nDataBit = dataPin.nBit;
					m_MCPEncoders[m_nMCPEncoderCount].nLastState = 0x03;
					m_MCPEncoders[m_nMCPEncoderCount].nAccumulator = 0;
					m_nMCPEncoderCount++;

					LOGDBG("4-Row encoder %u: clock=%s data=%s", i, pClk, pDat);
				}
			}

			// 4-Row navigation: buttons or encoder
			const char *pNavMode = m_pConfig->GetPropertyString("4RowNavMode", "buttons");
			if (strcmp(pNavMode, "encoder") == 0)
			{
				// Nav encoder mode: one quadrature encoder (twist) + click pin (back)
				m_bNavEncoderMode = true;

				const char *pChA = m_pConfig->GetPropertyString("4RowNavEncChAPin", "0");
				const char *pChB = m_pConfig->GetPropertyString("4RowNavEncChBPin", "0");
				const char *pClk = m_pConfig->GetPropertyString("4RowNavEncClickPin", "0");

				TMCPPin chAPin = ParseMCPPin(pChA);
				TMCPPin chBPin = ParseMCPPin(pChB);

				if (chAPin.bValid && chBPin.bValid && m_nMCPEncoderCount < MAX_MCP_ENCODERS)
				{
					m_nNavEncoderIndex = m_nMCPEncoderCount;
					m_MCPEncoders[m_nMCPEncoderCount].bClockIsPortA = chAPin.bIsPortA;
					m_MCPEncoders[m_nMCPEncoderCount].nClockBit     = chAPin.nBit;
					m_MCPEncoders[m_nMCPEncoderCount].bDataIsPortA  = chBPin.bIsPortA;
					m_MCPEncoders[m_nMCPEncoderCount].nDataBit      = chBPin.nBit;
					m_MCPEncoders[m_nMCPEncoderCount].nLastState    = 0x03;
					m_MCPEncoders[m_nMCPEncoderCount].nAccumulator  = 0;
					m_nMCPEncoderCount++;
					LOGDBG("4-Row nav encoder: chA=%s chB=%s", pChA, pChB);
				}

				// Nav encoder click → Back
				AddMCPButtonBinding(pClk, CUIMenu::MenuEventBack);
				LOGDBG("4-Row nav encoder click (back): pin=%s", pClk);
			}
			else
			{
				// Button mode: original 3 separate nav buttons
				AddMCPButtonBinding(m_pConfig->GetPropertyString("4RowBtnBackPin", "0"),
						    CUIMenu::MenuEventBack);
				AddMCPButtonBinding(m_pConfig->GetPropertyString("4RowBtnUpPin", "0"),
						    CUIMenu::MenuEventStepUp);
				AddMCPButtonBinding(m_pConfig->GetPropertyString("4RowBtnDownPin", "0"),
						    CUIMenu::MenuEventStepDown);
			}

			// Home button always present regardless of nav mode
			AddMCPButtonBinding(m_pConfig->GetPropertyString("4RowBtnHomePin", "0"),
					    CUIMenu::MenuEventHome);

			// 4-Row encoder click buttons
			const char *encClickPins[] = {
				"4RowEncClk1Pin", "4RowEncClk2Pin",
				"4RowEncClk3Pin", "4RowEncClk4Pin"
			};
			for (unsigned i = 0; i < 4; i++)
			{
				const char *pPin = m_pConfig->GetPropertyString(encClickPins[i], "0");
				TMCPPin pin = ParseMCPPin(pPin);
				if (pin.bValid && m_nMCPEncoderClickCount < MAX_ENCODER_CLICKS)
				{
					m_MCPEncoderClicks[m_nMCPEncoderClickCount].bIsPortA = pin.bIsPortA;
					m_MCPEncoderClicks[m_nMCPEncoderClickCount].nBit = pin.nBit;
					m_MCPEncoderClicks[m_nMCPEncoderClickCount].nEncoderIndex = i;
					m_nMCPEncoderClickCount++;
					LOGDBG("4-Row encoder click %u: pin=%s", i, pPin);
				}
			}

			LOGDBG("4-Row UI: %u encoders, %u buttons, %u encoder clicks",
			       m_nMCPEncoderCount, m_nMCPButtonCount, m_nMCPEncoderClickCount);
		}
	}

	// SSD1309 OLED hardware reset (Option B: use CSSD1309Display for reset only)
	unsigned nOLEDResetPin = m_pConfig->GetOLEDResetGPIO();
	if (nOLEDResetPin != 0)
	{
		m_pSSD1309 = new CSSD1309Display(m_pI2CMaster,
						 nOLEDResetPin,
						 m_pConfig->GetSSD1306LCDI2CAddress());
		assert(m_pSSD1309);

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
		LCDWrite("DreamDexed\nLoading...");
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
					      m_pGPIOManager);
		assert(m_pRotaryEncoder);

		if (!m_pRotaryEncoder->Initialize())
		{
			return false;
		}

		m_pRotaryEncoder->RegisterEventHandler(EncoderEventStub, this);

		LOGDBG("Rotary encoder initialized");
	}

	m_Menu.EventHandler(CUIMenu::MenuEventUpdate);

	// Initialize 4-Row UI engine if in 4-row mode
	if (m_bUse4RowUI)
	{
		m_pUI4Row = new CUI4Row(this, m_pMiniDexed, m_pConfig);
		assert(m_pUI4Row);
		LOGDBG("4-Row UI engine initialized");
	}

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

	// 4-Row UI rendering
	if (m_bUse4RowUI && m_pUI4Row && m_pSSD1306)
	{
		m_pUI4Row->Render(m_pSSD1306);
	}
}

void CUserInterface::PollMCP()
{
	assert(m_pMCP);

	// Read both MCP23017 ports
	uint8_t nPortA = m_pMCP->ReadPortA();
	uint8_t nPortB = m_pMCP->ReadPortB();

	unsigned nPulsePerStep = m_pConfig->GetEncoderPulsePerStep();
	if (nPulsePerStep == 0)
	{
		nPulsePerStep = 1;
	}

	// --- Decode configured encoders ---
	for (unsigned enc = 0; enc < m_nMCPEncoderCount; enc++)
	{
		TMCPEncoderBinding &binding = m_MCPEncoders[enc];

		// Read clock and data pins from their respective ports
		uint8_t nClockPort = binding.bClockIsPortA ? nPortA : nPortB;
		uint8_t nDataPort = binding.bDataIsPortA ? nPortA : nPortB;

		uint8_t nRawClock = (nClockPort >> binding.nClockBit) & 0x01;
		uint8_t nRawData = (nDataPort >> binding.nDataBit) & 0x01;

		// Current state: Data in bit1, Clock in bit0
		uint8_t nCurrentState = (nRawData << 1) | nRawClock;
		uint8_t nPrevState = binding.nLastState;

		if (nCurrentState != nPrevState)
		{
			// Quadrature state table for direction detection
			// State transitions: 00→01→11→10 = CW, 00→10→11→01 = CCW
			static const int8_t quadTable[16] = {
				 0, -1,  1,  0,
				 1,  0,  0, -1,
				-1,  0,  0,  1,
				 0,  1, -1,  0
			};

			int8_t nDirection = quadTable[(nPrevState << 2) | nCurrentState];
			binding.nAccumulator += nDirection;

			// Only emit event when accumulator reaches threshold
			if (binding.nAccumulator >= (int)nPulsePerStep)
			{
				binding.nAccumulator = 0;
				if (m_bUse4RowUI && m_pUI4Row)
				{
					if (m_bNavEncoderMode && enc == m_nNavEncoderIndex)
						m_pUI4Row->OnScrollDown(); // CW = scroll down
					else
						m_pUI4Row->OnEncoderRotate(enc, 1);
				}
				else
				{
					m_Menu.EventHandler(CUIMenu::MenuEventStepUp);
				}
			}
			else if (binding.nAccumulator <= -(int)nPulsePerStep)
			{
				binding.nAccumulator = 0;
				if (m_bUse4RowUI && m_pUI4Row)
				{
					if (m_bNavEncoderMode && enc == m_nNavEncoderIndex)
						m_pUI4Row->OnScrollUp(); // CCW = scroll up
					else
						m_pUI4Row->OnEncoderRotate(enc, -1);
				}
				else
				{
					m_Menu.EventHandler(CUIMenu::MenuEventStepDown);
				}
			}

			binding.nLastState = nCurrentState;
		}
	}

	// --- Check configured buttons (active-low, falling edge) ---
	uint8_t nPressedA = m_nLastPortA & ~nPortA;  // bits that went HIGH→LOW
	uint8_t nPressedB = m_nLastPortB & ~nPortB;

	for (unsigned btn = 0; btn < m_nMCPButtonCount; btn++)
	{
		uint8_t nPressed = m_MCPButtons[btn].bIsPortA ? nPressedA : nPressedB;
		if (nPressed & (1 << m_MCPButtons[btn].nBit))
		{
			if (m_bUse4RowUI && m_pUI4Row)
			{
				// Route to 4-row UI
				switch (m_MCPButtons[btn].Event)
				{
				case CUIMenu::MenuEventBack:
					m_pUI4Row->OnBack();
					break;
				case CUIMenu::MenuEventStepUp:
					m_pUI4Row->OnScrollUp();
					break;
				case CUIMenu::MenuEventStepDown:
					m_pUI4Row->OnScrollDown();
					break;
				default:
					break;
				}
			}
			else
			{
				m_Menu.EventHandler(m_MCPButtons[btn].Event);
			}
		}
	}
	// --- Encoder click handling + long-hold (backspace in text input) ---
	// For encoder 1 in TextInput mode we use release-based detection to
	// distinguish short click (advance cursor) from long hold (backspace).
	// All other encoders keep the standard immediate click-on-press behaviour.
	if (m_bUse4RowUI && m_pUI4Row)
	{
		static unsigned s_nHeldEnc    = ~0u;   // index of currently held enc click (~0=none)
		static unsigned s_nHeldStart  = 0;     // tick when press began
		static bool     s_bLongFired  = false; // true once long-hold has fired this press

		const unsigned LONG_HOLD_US = 600000;  // 600ms threshold

		bool bAnyEncHeld = false;

		for (unsigned clk = 0; clk < m_nMCPEncoderClickCount; clk++)
		{
			uint8_t nPort    = m_MCPEncoderClicks[clk].bIsPortA ? nPortA : nPortB;
			uint8_t nPressed = m_MCPEncoderClicks[clk].bIsPortA ? nPressedA : nPressedB;
			bool    bHeld    = !(nPort & (1 << m_MCPEncoderClicks[clk].nBit)); // active-low
			bool    bJustPressed = (nPressed & (1 << m_MCPEncoderClicks[clk].nBit)) != 0;
			unsigned nEncIdx = m_MCPEncoderClicks[clk].nEncoderIndex;

			if (bHeld)
			{
				bAnyEncHeld = true;
				unsigned nNow = CTimer::Get()->GetClockTicks();

				if (s_nHeldEnc != clk)
				{
					// Fresh press
					s_nHeldEnc   = clk;
					s_nHeldStart = nNow;
					s_bLongFired = false;

					// Immediate click for non-enc1 (standard behaviour)
					if (bJustPressed && nEncIdx != 1)
					{
						m_pUI4Row->OnEncoderClick(nEncIdx);
					}
				}
				else if (!s_bLongFired && (nNow - s_nHeldStart >= LONG_HOLD_US))
				{
					// Long hold threshold reached — fire long-hold (enc1 = backspace)
					s_bLongFired = true;
					m_pUI4Row->OnEncoderLongHold(nEncIdx);
				}
				break; // only one encoder held at a time
			}
			else if (s_nHeldEnc == clk)
			{
				// Encoder was just released
				if (!s_bLongFired)
				{
					// Short press released → fire click for enc1
					// (non-enc1 already fired on press)
					if (nEncIdx == 1)
					{
						m_pUI4Row->OnEncoderClick(nEncIdx);
					}
				}
				// Reset state
				s_nHeldEnc   = ~0u;
				s_bLongFired = false;
			}
		}

		if (!bAnyEncHeld && s_nHeldEnc != ~0u)
		{
			// Fallback release detection (loop didn't find a match)
			s_nHeldEnc   = ~0u;
			s_bLongFired = false;
		}
	}

	// --- Auto-repeat for held Up/Down buttons (4-row mode) ---
	if (m_bUse4RowUI && m_pUI4Row)
	{
		static unsigned s_nHeldBtn = ~0u;       // index of held button (~0 = none)
		static unsigned s_nHeldStart = 0;       // tick when first held
		static unsigned s_nLastRepeat = 0;      // tick of last repeat event
		static const unsigned REPEAT_DELAY_US  = 400000;  // 400ms before repeats start
		static const unsigned REPEAT_RATE_US   = 120000;  // 120ms between repeats

		bool bAnyHeld = false;
		for (unsigned btn = 0; btn < m_nMCPButtonCount; btn++)
		{
			// Only auto-repeat Up and Down
			if (m_MCPButtons[btn].Event != CUIMenu::MenuEventStepUp &&
			    m_MCPButtons[btn].Event != CUIMenu::MenuEventStepDown)
			{
				continue;
			}

			uint8_t nPort = m_MCPButtons[btn].bIsPortA ? nPortA : nPortB;
			bool bHeld = !(nPort & (1 << m_MCPButtons[btn].nBit)); // active-low

			if (bHeld)
			{
				bAnyHeld = true;
				unsigned nNow = CTimer::Get()->GetClockTicks();

				if (s_nHeldBtn != btn)
				{
					// New button held — start repeat timer
					s_nHeldBtn = btn;
					s_nHeldStart = nNow;
					s_nLastRepeat = nNow;
				}
				else if (nNow - s_nHeldStart >= REPEAT_DELAY_US &&
				         nNow - s_nLastRepeat >= REPEAT_RATE_US)
				{
					// Fire repeat event
					s_nLastRepeat = nNow;
					if (m_MCPButtons[btn].Event == CUIMenu::MenuEventStepUp)
					{
						m_pUI4Row->OnScrollUp();
					}
					else
					{
						m_pUI4Row->OnScrollDown();
					}
				}
				break; // only one button can be held at a time
			}
		}
		if (!bAnyHeld)
		{
			s_nHeldBtn = ~0u;
		}
	}

	m_nLastPortA = nPortA;
	m_nLastPortB = nPortB;
}

void CUserInterface::ParameterChanged()
{
	m_Menu.EventHandler(CUIMenu::MenuEventUpdateParameter);

	if (m_bUse4RowUI && m_pUI4Row)
	{
		m_pUI4Row->OnParameterChanged();
	}
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

	// In 4-row mode, skip classic display writing — CUI4Row handles rendering
	if (m_bUse4RowUI)
	{
		return;
	}

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
