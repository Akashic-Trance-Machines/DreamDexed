//
// ui4row.cpp
//
// DreamDexed - 4-Row Hierarchical UI Engine
// Copyright (C) 2024  The DreamDexed Team
//
// Implements the 4-row hierarchical menu UI for 128x64 OLED displays.
// See 4RowUI.md for the full specification.
//
#include "ui4row.h"

extern "C" {
#include "u8g2/u8g2.h"
}

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include <circle/logger.h>
#include <circle/net/ipaddress.h>
#include <circle/string.h>
#include <display/ssd1306device.h>

#include "config.h"
#include "mididevice.h"
#include "minidexed.h"
#include "performanceconfig.h"
#include "sdfilter.h"
#include "status.h"
#include "userinterface.h"
#include "effect.h"
#include "bus.h"
#include <circle/timer.h>

LOGMODULE("ui4row");

// Debounce timer (1 tick = 10ms typically)
static unsigned s_nLastButtonTick = 0;

static bool CheckDebounce()
{
	unsigned nNow = CTimer::Get()->GetClockTicks();
	// 20 ticks = ~200ms debounce
	if ((unsigned)(nNow - s_nLastButtonTick) < 20)
		return false;
	s_nLastButtonTick = nNow;
	return true;
}

// --- Display constants (pixel-precise layout per 4RowUI.md spec) ---
static const unsigned SCREEN_ROWS = 4;
static const unsigned SCREEN_WIDTH_PX = 128;
static const unsigned SCREEN_HEIGHT_PX = 64;
static const unsigned ROW_HEIGHT_PX = 16;

// Scrollbar (left zone: x 0–1)
static const unsigned SCROLLBAR_WIDTH_PX = 2;
static const unsigned SCROLLBAR_MIN_HEIGHT = 4;

// Text (middle zone: x 3–121)
static const unsigned TEXT_X = 3;
static const unsigned TEXT_BASELINE[4] = {14, 30, 46, 62};

// Action indicator (right zone: x 122–127)
static const unsigned ACTION_BOX_X = 122;
static const unsigned ACTION_BOX_W = 6;
static const unsigned ACTION_BOX_H = 15;
static const unsigned ACTION_ICON_X = 123;
static const unsigned ACTION_ICON_W = 4;
static const unsigned ACTION_ICON_H = 7;
static const unsigned ACTION_BOX_Y[4]  = {1, 17, 33, 49};
static const unsigned ACTION_ICON_Y[4] = {5, 21, 37, 53};

// Action indicator bitmap: right-pointing triangle ▶, 4px wide × 7px tall
// XBM format: LSB first within each byte
static const unsigned char s_ActionIcon[] = {0x01, 0x03, 0x07, 0x0f, 0x07, 0x03, 0x01};

// ============================================================================
// Constructor
// ============================================================================

CUI4Row::CUI4Row(CUserInterface *pUI, CMiniDexed *pMiniDexed, CConfig *pConfig) :
m_pUI{pUI},
m_pMiniDexed{pMiniDexed},
m_pConfig{pConfig},
m_nMenuDepth{0},
m_nScrollIndex{0},
m_bDirty{true},
m_nActiveTG{0},
m_nActiveOP{0},
m_nActiveModSource{0},
m_nActiveBus{0},
m_nActiveFxBus{0},
m_nActiveFxSlot{0},
m_nSavedMasterVolume{0},
m_bMasterMuted{false}
{
	memset(m_nMenuStack, 0, sizeof(m_nMenuStack));
	memset(m_nScrollStack, 0, sizeof(m_nScrollStack));
	memset(&m_CurrentPage, 0, sizeof(m_CurrentPage));
	memset(m_szValueBuf, 0, sizeof(m_szValueBuf));

	// Initialize with the Home page
	m_nMenuStack[0] = MenuHome;
	BuildCurrentPage();

	// Initialize u8g2 in full-buffer mode (no I2C — buffer only)
	u8g2_Setup_ssd1306_i2c_128x64_noname_f(
		&m_u8g2, U8G2_R0,
		u8x8_byte_circle_noop,
		u8x8_gpio_and_delay_circle
	);
	// DO NOT call u8g2_InitDisplay() here — it sends I2C init commands
	// with CTimer delays that crash during early boot (timer not ready).
	// Instead, directly trigger DISPLAY_SETUP_MEMORY to populate the
	// internal display_info (tile_width, tile_height, pixel dimensions)
	// that all draw functions depend on — no I2C, no delays.
	u8x8_t *pU8x8 = u8g2_GetU8x8(&m_u8g2);
	pU8x8->display_cb(pU8x8, U8X8_MSG_DISPLAY_SETUP_MEMORY, 0, NULL);

	LOGDBG("4-Row UI initialized (u8g2 rendering)");
}

// ============================================================================
// Navigation
// ============================================================================

void CUI4Row::OnBack()
{
	if (!CheckDebounce()) return;

	if (m_nMenuDepth == 0)
		return; // can't go back from Home

	m_nMenuDepth--;
	m_nScrollIndex = m_nScrollStack[m_nMenuDepth];
	BuildCurrentPage();
	m_bDirty = true;
}

void CUI4Row::OnScrollUp()
{
	if (!CheckDebounce()) return;

	if (m_nScrollIndex > 0)
	{
		m_nScrollIndex--;
		m_bDirty = true;
	}
}

void CUI4Row::OnScrollDown()
{
	if (!CheckDebounce()) return;

	if (m_CurrentPage.nRowCount > SCREEN_ROWS &&
	    m_nScrollIndex < m_CurrentPage.nRowCount - SCREEN_ROWS)
	{
		m_nScrollIndex++;
		m_bDirty = true;
	}
}

// ============================================================================
// Encoder Rotate — change parameter values
// ============================================================================

void CUI4Row::OnEncoderRotate(unsigned nEncoder, int nDirection)
{
	unsigned nItemIndex = m_nScrollIndex + nEncoder;
	if (nItemIndex >= m_CurrentPage.nRowCount)
		return;

	TMenuRow &row = m_CurrentPage.Rows[nItemIndex];

	// Only properties respond to encoder rotation
	if (row.Type != RowTypeProperty)
		return;

	unsigned nCurrentMenu = m_nMenuStack[m_nMenuDepth];

	if (nCurrentMenu == MenuPerformance)
	{
		// Row indices within Performance page (0-based item index)
		switch (nItemIndex)
		{
		case 0: // Bank
		{
			int nBank = m_pMiniDexed->GetPerformanceBank();
			int nLastBank = m_pMiniDexed->GetLastPerformanceBank();

			if (nDirection > 0)
			{
				int nStart = nBank;
				do {
					nBank++;
					if (nBank > nLastBank)
						nBank = 0;
				} while (!m_pMiniDexed->IsValidPerformanceBank(nBank) && nBank != nStart);
			}
			else
			{
				int nStart = nBank;
				do {
					nBank--;
					if (nBank < 0)
						nBank = nLastBank;
				} while (!m_pMiniDexed->IsValidPerformanceBank(nBank) && nBank != nStart);
			}
			m_pMiniDexed->SetParameter(CMiniDexed::ParameterPerformanceBank, nBank);
			m_pMiniDexed->SetFirstPerformance();
			break;
		}
		case 1: // Perf
		{
			int nPerf = m_pMiniDexed->GetActualPerformanceID();
			int nLastPerf = m_pMiniDexed->GetLastPerformance();

			if (nDirection > 0)
			{
				int nStart = nPerf;
				do {
					nPerf++;
					if (nPerf > nLastPerf)
						nPerf = 0;
				} while (!m_pMiniDexed->IsValidPerformance(nPerf) && nPerf != nStart);
			}
			else
			{
				int nStart = nPerf;
				do {
					nPerf--;
					if (nPerf < 0)
						nPerf = nLastPerf;
				} while (!m_pMiniDexed->IsValidPerformance(nPerf) && nPerf != nStart);
			}
			// Safety: only set if valid (prevents assertion in GetPerformanceName)
			if (m_pMiniDexed->IsValidPerformance(nPerf))
			{
				m_pMiniDexed->SetNewPerformance(nPerf);
			}
			break;
		}
		case 4: // PCCH
		{
			int nValue = m_pMiniDexed->GetParameter(CMiniDexed::ParameterPerformanceSelectChannel);
			nValue += nDirection;
			if (nValue < 0)
				nValue = 0;
			if (nValue > (int)CMIDIDevice::Disabled)
				nValue = (int)CMIDIDevice::Disabled;
			m_pMiniDexed->SetParameter(CMiniDexed::ParameterPerformanceSelectChannel, nValue);
			break;
		}
		case 5: // Design Filter
		{
			int nValue = m_pMiniDexed->GetParameter(CMiniDexed::ParameterSDFilter);
			int nMax = SDFilter::get_maximum(m_pConfig->GetToneGenerators());
			nValue += nDirection;
			if (nValue < 0)
				nValue = 0;
			if (nValue > nMax)
				nValue = nMax;
			m_pMiniDexed->SetParameter(CMiniDexed::ParameterSDFilter, nValue);
			break;
		}
		default:
			break;
		}

		// Rebuild page to show updated values
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoices)
	{
		switch (nItemIndex)
		{
		case 0: // TG selector
		{
			int nOld = (int)m_nActiveTG;
			int nNew = nOld + nDirection;
			int nMax = (int)m_pConfig->GetToneGenerators() - 1;
			if (nNew < 0) nNew = 0;
			if (nNew > nMax) nNew = nMax;
			if (nNew != nOld)
			{
				m_pMiniDexed->notesOff(0, m_nActiveTG);
				m_nActiveTG = (unsigned)nNew;
			}
			break;
		}
		case 1: // Bank
		{
			int nBank = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterVoiceBank, m_nActiveTG);
			nBank += nDirection;
			if (nBank < 0) nBank = 0;
			m_pMiniDexed->SetTGParameter(CMiniDexed::TGParameterVoiceBank, nBank, m_nActiveTG);
			break;
		}
		case 2: // Voice
		{
			int nProg = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterProgram, m_nActiveTG);
			nProg += nDirection;
			if (nProg < 0) nProg = 0;
			if (nProg > 31) nProg = 31;
			m_pMiniDexed->SetTGParameter(CMiniDexed::TGParameterProgram, nProg, m_nActiveTG);
			break;
		}
		default:
		{
			// Rows 3-9: Volume, Pan, FX1-Send, FX2-Send, Detune, Cutoff, Resonance
			// Rows 13, 14: Poly/Mono, TG-Link
			static const struct { unsigned row; CMiniDexed::TTGParameter param; int nMin; int nMax; int nStep; } voiceParams[] = {
				{3,  CMiniDexed::TGParameterVolume,      0, 127, 8},
				{4,  CMiniDexed::TGParameterPan,          0, 127, 8},
				{5,  CMiniDexed::TGParameterFX1Send,       0, 99, 1},
				{6,  CMiniDexed::TGParameterFX2Send,       0, 99, 1},
				{7,  CMiniDexed::TGParameterMasterTune,  -99, 99, 1},
				{8,  CMiniDexed::TGParameterCutoff,        0, 99, 1},
				{9,  CMiniDexed::TGParameterResonance,     0, 99, 1},
				{13, CMiniDexed::TGParameterMonoMode,      0,  1, 1},
				{14, CMiniDexed::TGParameterTGLink,        0,  4, 1},
			};
			for (unsigned i = 0; i < sizeof(voiceParams)/sizeof(voiceParams[0]); i++)
			{
				if (nItemIndex == voiceParams[i].row)
				{
					int nVal = m_pMiniDexed->GetTGParameter(voiceParams[i].param, m_nActiveTG);
					nVal += nDirection * voiceParams[i].nStep;
					if (nVal < voiceParams[i].nMin) nVal = voiceParams[i].nMin;
					if (nVal > voiceParams[i].nMax) nVal = voiceParams[i].nMax;
					m_pMiniDexed->SetTGParameter(voiceParams[i].param, nVal, m_nActiveTG);
					break;
				}
			}
			break;
		}
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoicesPitchBend)
	{
		CMiniDexed::TTGParameter param = (nItemIndex == 0) ?
			CMiniDexed::TGParameterPitchBendRange : CMiniDexed::TGParameterPitchBendStep;
		int nVal = m_pMiniDexed->GetTGParameter(param, m_nActiveTG);
		nVal += nDirection;
		if (nVal < 0) nVal = 0;
		if (nVal > 12) nVal = 12;
		m_pMiniDexed->SetTGParameter(param, nVal, m_nActiveTG);
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoicesPortamento)
	{
		CMiniDexed::TTGParameter params[] = {
			CMiniDexed::TGParameterPortamentoMode,
			CMiniDexed::TGParameterPortamentoGlissando,
			CMiniDexed::TGParameterPortamentoTime
		};
		int limits[][2] = {{0, 1}, {0, 1}, {0, 99}};
		if (nItemIndex < 3)
		{
			int nVal = m_pMiniDexed->GetTGParameter(params[nItemIndex], m_nActiveTG);
			nVal += nDirection;
			if (nVal < limits[nItemIndex][0]) nVal = limits[nItemIndex][0];
			if (nVal > limits[nItemIndex][1]) nVal = limits[nItemIndex][1];
			m_pMiniDexed->SetTGParameter(params[nItemIndex], nVal, m_nActiveTG);
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoicesNoteLimit)
	{
		CMiniDexed::TTGParameter params[] = {
			CMiniDexed::TGParameterNoteLimitLow,
			CMiniDexed::TGParameterNoteLimitHigh,
			CMiniDexed::TGParameterNoteShift
		};
		int limits[][2] = {{0, 127}, {0, 127}, {-24, 24}};
		if (nItemIndex < 3)
		{
			int nVal = m_pMiniDexed->GetTGParameter(params[nItemIndex], m_nActiveTG);
			nVal += nDirection;
			if (nVal < limits[nItemIndex][0]) nVal = limits[nItemIndex][0];
			if (nVal > limits[nItemIndex][1]) nVal = limits[nItemIndex][1];
			m_pMiniDexed->SetTGParameter(params[nItemIndex], nVal, m_nActiveTG);
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoicesModulation)
	{
		if (nItemIndex == 0) // Source selector
		{
			int nNew = (int)m_nActiveModSource + nDirection;
			if (nNew < 0) nNew = 0;
			if (nNew > 3) nNew = 3;
			m_nActiveModSource = (unsigned)nNew;
		}
		else if (nItemIndex >= 1 && nItemIndex <= 4)
		{
			unsigned nBase = CMiniDexed::TGParameterMWRange + m_nActiveModSource * 4;
			CMiniDexed::TTGParameter param = (CMiniDexed::TTGParameter)(nBase + nItemIndex - 1);
			int nVal = m_pMiniDexed->GetTGParameter(param, m_nActiveTG);
			nVal += nDirection;
			int nMax = (nItemIndex == 1) ? 99 : 1; // Range=0-99, Pitch/Amp/EG=0-1
			if (nVal < 0) nVal = 0;
			if (nVal > nMax) nVal = nMax;
			m_pMiniDexed->SetTGParameter(param, nVal, m_nActiveTG);
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoicesMIDI)
	{
		CMiniDexed::TTGParameter params[] = {
			CMiniDexed::TGParameterMIDIChannel,
			CMiniDexed::TGParameterSysExChannel,
			CMiniDexed::TGParameterSysExEnable,
			CMiniDexed::TGParameterMIDIRxSustain,
			CMiniDexed::TGParameterMIDIRxPortamento,
			CMiniDexed::TGParameterMIDIRxSostenuto,
			CMiniDexed::TGParameterMIDIRxHold2,
		};
		int limits[][2] = {{0, 16}, {0, 15}, {0, 1}, {0, 1}, {0, 1}, {0, 1}, {0, 1}};
		if (nItemIndex < 7)
		{
			int nVal = m_pMiniDexed->GetTGParameter(params[nItemIndex], m_nActiveTG);
			nVal += nDirection;
			if (nVal < limits[nItemIndex][0]) nVal = limits[nItemIndex][0];
			if (nVal > limits[nItemIndex][1]) nVal = limits[nItemIndex][1];
			m_pMiniDexed->SetTGParameter(params[nItemIndex], nVal, m_nActiveTG);
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoicesEQ)
	{
		CMiniDexed::TTGParameter params[] = {
			CMiniDexed::TGParameterEQLow,
			CMiniDexed::TGParameterEQMid,
			CMiniDexed::TGParameterEQHigh,
			CMiniDexed::TGParameterEQGain,
			CMiniDexed::TGParameterEQLowMidFreq,
			CMiniDexed::TGParameterEQMidHighFreq,
			CMiniDexed::TGParameterEQPreLowcut,
			CMiniDexed::TGParameterEQPreHighcut,
		};
		int limits[][2] = {{-24,24},{-24,24},{-24,24},{-24,24},{0,46},{28,59},{0,60},{0,60}};
		if (nItemIndex < 8)
		{
			int nVal = m_pMiniDexed->GetTGParameter(params[nItemIndex], m_nActiveTG);
			nVal += nDirection;
			if (nVal < limits[nItemIndex][0]) nVal = limits[nItemIndex][0];
			if (nVal > limits[nItemIndex][1]) nVal = limits[nItemIndex][1];
			m_pMiniDexed->SetTGParameter(params[nItemIndex], nVal, m_nActiveTG);
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoicesCompressor)
	{
		CMiniDexed::TTGParameter params[] = {
			CMiniDexed::TGParameterCompressorEnable,
			CMiniDexed::TGParameterCompressorPreGain,
			CMiniDexed::TGParameterCompressorThresh,
			CMiniDexed::TGParameterCompressorRatio,
			CMiniDexed::TGParameterCompressorAttack,
			CMiniDexed::TGParameterCompressorRelease,
			CMiniDexed::TGParameterCompressorMakeupGain,
		};
		int limits[][2] = {{0,1},{-20,20},{-60,0},{1,20},{0,1000},{0,2000},{-20,20}};
		int steps[] = {1, 1, 1, 1, 5, 5, 1};
		if (nItemIndex < 7)
		{
			int nVal = m_pMiniDexed->GetTGParameter(params[nItemIndex], m_nActiveTG);
			nVal += nDirection * steps[nItemIndex];
			if (nVal < limits[nItemIndex][0]) nVal = limits[nItemIndex][0];
			if (nVal > limits[nItemIndex][1]) nVal = limits[nItemIndex][1];
			m_pMiniDexed->SetTGParameter(params[nItemIndex], nVal, m_nActiveTG);
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoicesEditVoice)
	{
		// Row 0: Algorithm (0-31), Row 1: Feedback (0-7)
		// Row 2: Operators submenu (skip)
		// Rows 3-10: P EG R1-R4/L1-L4 (0-99)
		// Row 11: Osc Key Sync (0-1)
		// Rows 12-17: LFO params
		// Row 18: P Mod Sens (0-7), Row 19: Transpose (0-48)
		if (nItemIndex == 0) // Algorithm
		{
			int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_ALGORITHM, 6, m_nActiveTG);
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > 31) nVal = 31;
			m_pMiniDexed->SetVoiceParameter(DEXED_ALGORITHM, nVal, 6, m_nActiveTG);
		}
		else if (nItemIndex == 1) // Feedback
		{
			int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_FEEDBACK, 6, m_nActiveTG);
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > 7) nVal = 7;
			m_pMiniDexed->SetVoiceParameter(DEXED_FEEDBACK, nVal, 6, m_nActiveTG);
		}
		else if (nItemIndex >= 3 && nItemIndex <= 10) // P EG R1-R4, L1-L4
		{
			int pegParams[] = {
				DEXED_PITCH_EG_R1, DEXED_PITCH_EG_R2, DEXED_PITCH_EG_R3, DEXED_PITCH_EG_R4,
				DEXED_PITCH_EG_L1, DEXED_PITCH_EG_L2, DEXED_PITCH_EG_L3, DEXED_PITCH_EG_L4,
			};
			int nParam = pegParams[nItemIndex - 3];
			int nVal = m_pMiniDexed->GetVoiceParameter(nParam, 6, m_nActiveTG);
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > 99) nVal = 99;
			m_pMiniDexed->SetVoiceParameter(nParam, nVal, 6, m_nActiveTG);
		}
		else if (nItemIndex == 11) // Osc Key Sync
		{
			int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_OSC_KEY_SYNC, 6, m_nActiveTG);
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > 1) nVal = 1;
			m_pMiniDexed->SetVoiceParameter(DEXED_OSC_KEY_SYNC, nVal, 6, m_nActiveTG);
		}
		else if (nItemIndex >= 12 && nItemIndex <= 17) // LFO params
		{
			int lfoParams[] = {
				DEXED_LFO_SPEED, DEXED_LFO_DELAY, DEXED_LFO_PITCH_MOD_DEP,
				DEXED_LFO_AMP_MOD_DEP, DEXED_LFO_SYNC, DEXED_LFO_WAVE,
			};
			int lfoMax[] = {99, 99, 99, 99, 1, 5};
			int idx = nItemIndex - 12;
			int nVal = m_pMiniDexed->GetVoiceParameter(lfoParams[idx], 6, m_nActiveTG);
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > lfoMax[idx]) nVal = lfoMax[idx];
			m_pMiniDexed->SetVoiceParameter(lfoParams[idx], nVal, 6, m_nActiveTG);
		}
		else if (nItemIndex == 18) // P Mod Sens
		{
			int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_LFO_PITCH_MOD_SENS, 6, m_nActiveTG);
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > 7) nVal = 7;
			m_pMiniDexed->SetVoiceParameter(DEXED_LFO_PITCH_MOD_SENS, nVal, 6, m_nActiveTG);
		}
		else if (nItemIndex == 19) // Transpose
		{
			int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_TRANSPOSE, 6, m_nActiveTG);
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > 48) nVal = 48;
			m_pMiniDexed->SetVoiceParameter(DEXED_TRANSPOSE, nVal, 6, m_nActiveTG);
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	else if (nCurrentMenu == MenuVoicesOperators)
	{
		if (nItemIndex == 0) // OP selector
		{
			int nNew = (int)m_nActiveOP + nDirection;
			if (nNew < 0) nNew = 0;
			if (nNew > 5) nNew = 5;
			m_nActiveOP = (unsigned)nNew;
		}
		else if (nItemIndex >= 1 && nItemIndex <= 22)
		{
			// OP parameter items (same order as OPItems in builder)
			int opParams[] = {
				DEXED_OP_OUTPUT_LEV, DEXED_OP_FREQ_COARSE, DEXED_OP_FREQ_FINE,
				DEXED_OP_OSC_DETUNE, DEXED_OP_OSC_MODE,
				DEXED_OP_EG_R1, DEXED_OP_EG_R2, DEXED_OP_EG_R3, DEXED_OP_EG_R4,
				DEXED_OP_EG_L1, DEXED_OP_EG_L2, DEXED_OP_EG_L3, DEXED_OP_EG_L4,
				DEXED_OP_LEV_SCL_BRK_PT, DEXED_OP_SCL_LEFT_DEPTH, DEXED_OP_SCL_RGHT_DEPTH,
				DEXED_OP_SCL_LEFT_CURVE, DEXED_OP_SCL_RGHT_CURVE,
				DEXED_OP_OSC_RATE_SCALE, DEXED_OP_AMP_MOD_SENS, DEXED_OP_KEY_VEL_SENS,
				DEXED_OP_ENABLE,
			};
			int opMax[] = {
				99, 31, 99, 14, 1,
				99, 99, 99, 99,
				99, 99, 99, 99,
				99, 99, 99,
				3, 3,
				7, 3, 7,
				1,
			};
			int idx = nItemIndex - 1;
			int nVal = m_pMiniDexed->GetVoiceParameter(opParams[idx], m_nActiveOP, m_nActiveTG);
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > opMax[idx]) nVal = opMax[idx];
			m_pMiniDexed->SetVoiceParameter(opParams[idx], nVal, m_nActiveOP, m_nActiveTG);
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	// Other menus: encoder rotation is a no-op for now

	// --- Phase 4: Effects menu rotation ---
	else if (nCurrentMenu == MenuEffects)
	{
		int nFX = GetActiveFxChainIndex();
		bool bIsMaster = (m_nActiveFxBus >= (unsigned)CConfig::BusFXChains);

		// New row order: 0=FxBus, 1=DryLevel(if SendFX), slot, alg, params
		// Compute row mapping:
		// Row 0: Fx bus selector (always)
		// Row 1: Dry Level (only if not MasterFX)
		// Row N: Fx slot selector
		// Row N+1: Fx algorithm selector
		// Row N+2...: algorithm params
		unsigned rowFxBus = 0;
		unsigned rowDryLevel = bIsMaster ? 999 : 1; // 999 = doesn't exist
		unsigned rowFxSlot = bIsMaster ? 1 : 2;
		unsigned rowFxAlg = rowFxSlot + 1;
		unsigned rowParamsStart = rowFxAlg + 1;

		if (nItemIndex == rowFxBus) // Fx bus selector
		{
			unsigned nMaxBus = CConfig::BusFXChains;
			int nNew = (int)m_nActiveFxBus + nDirection;
			if (nNew < 0) nNew = 0;
			if (nNew > (int)nMaxBus) nNew = (int)nMaxBus;
			if ((unsigned)nNew != m_nActiveFxBus)
			{
				m_nActiveFxBus = (unsigned)nNew;
				m_nActiveFxSlot = 0;
				m_nScrollIndex = 0;
			}
		}
		else if (!bIsMaster && nItemIndex == rowDryLevel) // Dry Level
		{
			int nVal = m_pMiniDexed->GetBusParameter(Bus::MixerDryLevel, m_nActiveBus);
			int nMax = Bus::s_Parameter[Bus::MixerDryLevel].Maximum;
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > nMax) nVal = nMax;
			m_pMiniDexed->SetBusParameter(Bus::MixerDryLevel, nVal, m_nActiveBus);
		}
		else if (nItemIndex == rowFxSlot) // Fx slot selector
		{
			int nNew = (int)m_nActiveFxSlot + nDirection;
			if (nNew < 0) nNew = 0;
			if (nNew > 2) nNew = 2;
			if ((unsigned)nNew != m_nActiveFxSlot)
			{
				m_nActiveFxSlot = (unsigned)nNew;
				m_nScrollIndex = 0;
			}
		}
		else if (nItemIndex == rowFxAlg) // Fx algorithm selector
		{
			FX::Parameter slotParam = (FX::Parameter)(FX::Parameter::Slot0 + m_nActiveFxSlot);
			int nAlg = m_pMiniDexed->GetFXParameter(slotParam, nFX);
			nAlg += nDirection;
			if (nAlg < 0) nAlg = 0;
			if (nAlg >= FX::effects_num) nAlg = FX::effects_num - 1;
			m_pMiniDexed->SetFXParameter(slotParam, nAlg, nFX);
		}
		else if (nItemIndex >= rowParamsStart) // Dynamic params
		{
			// Determine which param this row corresponds to
			FX::Parameter slotParam = (FX::Parameter)(FX::Parameter::Slot0 + m_nActiveFxSlot);
			int nAlg = m_pMiniDexed->GetFXParameter(slotParam, nFX);
			if (nAlg > 0 && nAlg < FX::effects_num)
			{
				int minID = FX::s_effects[nAlg].MinID;
				int maxID = FX::s_effects[nAlg].MaxID;
				int paramIdx = nItemIndex - rowParamsStart;

				// Build a param list: for most algorithms, all params from MinID to MaxID
				// For CloudSeed2, top-level params come first, then submenu entries (skip those)
				int paramID = minID + paramIdx;
				if (paramID >= minID && paramID <= maxID)
				{
					// Check if this is a CloudSeed2 sub-menu placeholder
					// (sub-menu rows are RowTypeMenuItem, handled in click handler)
					if (m_CurrentPage.Rows[nItemIndex].Type == RowTypeProperty)
					{
						FX::Parameter fp = (FX::Parameter)paramID;
						int nVal = m_pMiniDexed->GetFXParameter(fp, nFX);
						int nMin = FX::s_Parameter[fp].Minimum;
						int nMax = FX::s_Parameter[fp].Maximum;
						int nInc = FX::s_Parameter[fp].Increment;
						if (nInc < 1) nInc = 1;
						nVal += nDirection * nInc;
						if (nVal < nMin) nVal = nMin;
						if (nVal > nMax) nVal = nMax;
						m_pMiniDexed->SetFXParameter(fp, nVal, nFX);
					}
				}
			}
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	// --- Phase 4: CloudSeed2 sub-page rotation ---
	else if (nCurrentMenu >= MenuEffectsCS2Input && nCurrentMenu <= MenuEffectsCS2LowPass)
	{
		// Each CS2 sub-page has simple FX params
		if (nItemIndex < m_CurrentPage.nRowCount && m_CurrentPage.Rows[nItemIndex].Type == RowTypeProperty)
		{
			int nFX = GetActiveFxChainIndex();
			// The param ID is stored in the row data — we need to look it up
			// Each sub-page's params are defined in the builder; we use a table approach
			static const struct { unsigned menu; FX::Parameter params[8]; unsigned count; } CS2SubPages[] = {
				{MenuEffectsCS2Input, {FX::CloudSeed2Interpolation, FX::CloudSeed2InputMix, FX::CloudSeed2HighCutEnabled, FX::CloudSeed2HighCut, FX::CloudSeed2LowCutEnabled, FX::CloudSeed2LowCut}, 6},
				{MenuEffectsCS2Multitap, {FX::CloudSeed2TapEnabled, FX::CloudSeed2TapCount, FX::CloudSeed2TapDecay, FX::CloudSeed2TapPredelay, FX::CloudSeed2TapLength}, 5},
				{MenuEffectsCS2EarlyDiffusion, {FX::CloudSeed2EarlyDiffuseEnabled, FX::CloudSeed2EarlyDiffuseCount, FX::CloudSeed2EarlyDiffuseDelay, FX::CloudSeed2EarlyDiffuseFeedback, FX::CloudSeed2EarlyDiffuseModAmount, FX::CloudSeed2EarlyDiffuseModRate}, 6},
				{MenuEffectsCS2LateDiffusion, {FX::CloudSeed2LateDiffuseEnabled, FX::CloudSeed2LateDiffuseCount, FX::CloudSeed2LateDiffuseDelay, FX::CloudSeed2LateDiffuseFeedback, FX::CloudSeed2LateDiffuseModAmount, FX::CloudSeed2LateDiffuseModRate}, 6},
				{MenuEffectsCS2LateLines, {FX::CloudSeed2LateMode, FX::CloudSeed2LateLineCount, FX::CloudSeed2LateLineSize, FX::CloudSeed2LateLineDecay, FX::CloudSeed2LateLineModAmount, FX::CloudSeed2LateLineModRate}, 6},
				{MenuEffectsCS2LowShelf, {FX::CloudSeed2EqLowShelfEnabled, FX::CloudSeed2EqLowFreq, FX::CloudSeed2EqLowGain}, 3},
				{MenuEffectsCS2HighShelf, {FX::CloudSeed2EqHighShelfEnabled, FX::CloudSeed2EqHighFreq, FX::CloudSeed2EqHighGain}, 3},
				{MenuEffectsCS2LowPass, {FX::CloudSeed2EqLowpassEnabled, FX::CloudSeed2EqCutoff}, 2},
			};

			for (unsigned p = 0; p < sizeof(CS2SubPages)/sizeof(CS2SubPages[0]); p++)
			{
				if (CS2SubPages[p].menu == nCurrentMenu && nItemIndex < CS2SubPages[p].count)
				{
					FX::Parameter fp = CS2SubPages[p].params[nItemIndex];
					int nVal = m_pMiniDexed->GetFXParameter(fp, nFX);
					int nMin = FX::s_Parameter[fp].Minimum;
					int nMax = FX::s_Parameter[fp].Maximum;
					int nInc = FX::s_Parameter[fp].Increment;
					if (nInc < 1) nInc = 1;
					nVal += nDirection * nInc;
					if (nVal < nMin) nVal = nMin;
					if (nVal > nMax) nVal = nMax;
					m_pMiniDexed->SetFXParameter(fp, nVal, nFX);
					break;
				}
			}
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
	// --- Phase 5: Mixer menu rotation ---
	else if (nCurrentMenu == MenuMixer)
	{
		// Row 0: Master Volume, Row 1: Dry Level, Row 2: FX1 Return, Row 3: FX2 Return, Row 4: Return
		switch (nItemIndex)
		{
		case 0: // Master Volume
		{
			int nVal = m_pMiniDexed->GetParameter(CMiniDexed::ParameterMasterVolume);
			nVal += nDirection * 8;
			if (nVal < 0) nVal = 0;
			if (nVal > 127) nVal = 127;
			m_pMiniDexed->SetParameter(CMiniDexed::ParameterMasterVolume, nVal);
			if (m_bMasterMuted && nVal > 0)
				m_bMasterMuted = false; // unmute if user adjusts volume
			break;
		}
		case 1: // Dry Level
		{
			int nVal = m_pMiniDexed->GetBusParameter(Bus::MixerDryLevel, 0);
			int nMax = Bus::s_Parameter[Bus::MixerDryLevel].Maximum;
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > nMax) nVal = nMax;
			m_pMiniDexed->SetBusParameter(Bus::MixerDryLevel, nVal, 0);
			break;
		}
		case 2: // FX1 Return
		{
			int nVal = m_pMiniDexed->GetFXParameter(FX::ReturnLevel, 0);
			int nMax = FX::s_Parameter[FX::ReturnLevel].Maximum;
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > nMax) nVal = nMax;
			m_pMiniDexed->SetFXParameter(FX::ReturnLevel, nVal, 0);
			break;
		}
		case 3: // FX2 Return
		{
			int nVal = m_pMiniDexed->GetFXParameter(FX::ReturnLevel, 1);
			int nMax = FX::s_Parameter[FX::ReturnLevel].Maximum;
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > nMax) nVal = nMax;
			m_pMiniDexed->SetFXParameter(FX::ReturnLevel, nVal, 1);
			break;
		}
		case 4: // Return Level
		{
			int nVal = m_pMiniDexed->GetBusParameter(Bus::ReturnLevel, 0);
			int nMax = Bus::s_Parameter[Bus::ReturnLevel].Maximum;
			nVal += nDirection;
			if (nVal < 0) nVal = 0;
			if (nVal > nMax) nVal = nMax;
			m_pMiniDexed->SetBusParameter(Bus::ReturnLevel, nVal, 0);
			break;
		}
		}
		BuildCurrentPage();
		m_bDirty = true;
	}
}

// ============================================================================
// Encoder Click — trigger actions
// ============================================================================

void CUI4Row::OnEncoderClick(unsigned nEncoder)
{
	if (!CheckDebounce()) return;

	unsigned nItemIndex = m_nScrollIndex + nEncoder;
	if (nItemIndex >= m_CurrentPage.nRowCount)
		return;

	TMenuRow &row = m_CurrentPage.Rows[nItemIndex];
	unsigned nCurrentMenu = m_nMenuStack[m_nMenuDepth];

	// --- Handle "enter submenu" action ---
	if (row.Type == RowTypeMenuItem && row.Action == ActionEnterSubmenu)
	{
		// Save current scroll position
		m_nScrollStack[m_nMenuDepth] = m_nScrollIndex;

		if (m_nMenuDepth < MAX_MENU_DEPTH - 1)
		{
			m_nMenuDepth++;
			m_nScrollIndex = 0;

			// Determine which submenu to enter based on current page
			if (nCurrentMenu == MenuHome)
			{
				// Home menu items in order: Performance, Voices, Effects, Mixer, Status
				static const unsigned HomeSubmenus[] = {
					MenuPerformance, MenuVoices, MenuEffects, MenuMixer, MenuStatus
				};
				if (nItemIndex < 5)
					m_nMenuStack[m_nMenuDepth] = HomeSubmenus[nItemIndex];
				else
					m_nMenuStack[m_nMenuDepth] = MenuHome; // fallback
			}
			else if (nCurrentMenu == MenuPerformance)
			{
				// Performance submenu items: row 2 = Save, row 3 = Delete
				if (nItemIndex == 2)
					m_nMenuStack[m_nMenuDepth] = MenuSaveSubmenu;
				else if (nItemIndex == 3)
					m_nMenuStack[m_nMenuDepth] = MenuDeleteConfirm;
				else
					m_nMenuStack[m_nMenuDepth] = MenuHome; // fallback
			}
			else if (nCurrentMenu == MenuVoices)
			{
				// Voices submenus by row index
				static const struct { unsigned row; unsigned menu; } VoicesSubmenus[] = {
					{10, MenuVoicesPitchBend},
					{11, MenuVoicesPortamento},
					{12, MenuVoicesNoteLimit},
					{15, MenuVoicesModulation},
					{16, MenuVoicesMIDI},
					{17, MenuVoicesEQ},
					{18, MenuVoicesCompressor},
					{19, MenuVoicesEditVoice},
				};
				bool bFound = false;
				for (unsigned i = 0; i < sizeof(VoicesSubmenus)/sizeof(VoicesSubmenus[0]); i++)
				{
					if (nItemIndex == VoicesSubmenus[i].row)
					{
						m_nMenuStack[m_nMenuDepth] = VoicesSubmenus[i].menu;
						bFound = true;
						break;
					}
				}
				if (!bFound)
					m_nMenuStack[m_nMenuDepth] = MenuVoices; // fallback
			}
			else if (nCurrentMenu == MenuVoicesEditVoice)
			{
				// Edit Voice: row 2 = Operators
				if (nItemIndex == 2)
					m_nMenuStack[m_nMenuDepth] = MenuVoicesOperators;
				else
					m_nMenuStack[m_nMenuDepth] = MenuVoicesEditVoice; // fallback
			}
			else if (nCurrentMenu == MenuEffects)
			{
				// CloudSeed2 sub-page entries (they are RowTypeMenuItem rows)
				// These are built dynamically — we need to figure out which CS2 submenu
				// The builder stores CS2 submenu rows as MenuItem with specific known labels
				// We match by comparing the row label
				static const struct { const char *label; unsigned menu; } CS2SubMenuMap[] = {
					{"Input",            MenuEffectsCS2Input},
					{"Multitap Delay",   MenuEffectsCS2Multitap},
					{"Early Diffusion",  MenuEffectsCS2EarlyDiffusion},
					{"Late Diffusion",   MenuEffectsCS2LateDiffusion},
					{"Late Lines",       MenuEffectsCS2LateLines},
					{"Low Shelf",        MenuEffectsCS2LowShelf},
					{"High Shelf",       MenuEffectsCS2HighShelf},
					{"Low Pass",         MenuEffectsCS2LowPass},
				};
				bool bFound = false;
				if (row.pLabel)
				{
					for (unsigned i = 0; i < sizeof(CS2SubMenuMap)/sizeof(CS2SubMenuMap[0]); i++)
					{
						if (strcmp(row.pLabel, CS2SubMenuMap[i].label) == 0)
						{
							m_nMenuStack[m_nMenuDepth] = CS2SubMenuMap[i].menu;
							bFound = true;
							break;
						}
					}
				}
				if (!bFound)
					m_nMenuStack[m_nMenuDepth] = MenuEffects; // fallback
			}
			else
			{
				// Fallback: stay on current menu
				m_nMenuStack[m_nMenuDepth] = nCurrentMenu;
			}

			BuildCurrentPage();
			m_bDirty = true;
		}
		return;
	}

	// --- Handle action commands ---
	if (row.Type == RowTypeAction && row.Action == ActionCommand)
	{
		if (nCurrentMenu == MenuSaveSubmenu)
		{
			if (nItemIndex == 0) // Overwrite
			{
				m_pMiniDexed->SavePerformance(false);
				LOGDBG("Performance saved (overwrite)");
				// Go back to Performance page
				OnBack();
			}
			else if (nItemIndex == 2) // Save as default
			{
				m_pMiniDexed->SavePerformance(true);
				LOGDBG("Performance saved as default");
				OnBack();
			}
		}
		else if (nCurrentMenu == MenuDeleteConfirm)
		{
			if (nItemIndex == 1) // Confirm delete
			{
				int nPerfID = m_pMiniDexed->GetActualPerformanceID();
				m_pMiniDexed->DeletePerformance(nPerfID);
				LOGDBG("Performance %d deleted", nPerfID);
				// Pop back twice (Delete confirm → Performance → Home)
				// or just go back to Performance
				OnBack();
			}
			else if (nItemIndex == 2) // Cancel
			{
				OnBack();
			}
		}
		return;
	}

	// --- Phase 4: Effects toggle actions ---
	if (nCurrentMenu == MenuEffects && row.Action == ActionToggle)
	{
		int nFX = GetActiveFxChainIndex();
		bool bIsMaster = (m_nActiveFxBus >= (unsigned)CConfig::BusFXChains);
		unsigned rowFxBus = bIsMaster ? 0 : 1;
		unsigned rowFxSlot = rowFxBus + 1;

		if (nItemIndex == rowFxBus) // FX chain bypass
		{
			int nVal = m_pMiniDexed->GetFXParameter(FX::Bypass, nFX);
			m_pMiniDexed->SetFXParameter(FX::Bypass, nVal ? 0 : 1, nFX);
		}
		else if (nItemIndex == rowFxSlot) // Algorithm bypass
		{
			FX::Parameter slotParam = (FX::Parameter)(FX::Parameter::Slot0 + m_nActiveFxSlot);
			int nAlg = m_pMiniDexed->GetFXParameter(slotParam, nFX);
			if (nAlg > 0 && nAlg < FX::effects_num)
			{
				// Toggle the per-algorithm bypass
				int bypassParam = FX::s_effects[nAlg].MaxID; // Last param is always bypass
				int nVal = m_pMiniDexed->GetFXParameter((FX::Parameter)bypassParam, nFX);
				m_pMiniDexed->SetFXParameter((FX::Parameter)bypassParam, nVal ? 0 : 1, nFX);
			}
		}
		BuildCurrentPage();
		m_bDirty = true;
		return;
	}

	// --- Phase 5: Mixer mute toggle ---
	if (nCurrentMenu == MenuMixer && nItemIndex == 0 && row.Action == ActionToggle)
	{
		if (m_bMasterMuted)
		{
			// Unmute: restore saved volume
			m_pMiniDexed->SetParameter(CMiniDexed::ParameterMasterVolume, m_nSavedMasterVolume);
			m_bMasterMuted = false;
		}
		else
		{
			// Mute: save current volume and set to 0
			m_nSavedMasterVolume = m_pMiniDexed->GetParameter(CMiniDexed::ParameterMasterVolume);
			m_pMiniDexed->SetParameter(CMiniDexed::ParameterMasterVolume, 0);
			m_bMasterMuted = true;
		}
		BuildCurrentPage();
		m_bDirty = true;
		return;
	}

	// Status menu: all read-only, ignore clicks
	// Other menus: no action on click for property rows without action
}

void CUI4Row::OnParameterChanged()
{
	// Rebuild current page to reflect external changes
	BuildCurrentPage();
	m_bDirty = true;
}

// ============================================================================
// Page Builders
// ============================================================================

void CUI4Row::BuildCurrentPage()
{
	memset(&m_CurrentPage, 0, sizeof(m_CurrentPage));
	memset(m_szValueBuf, 0, sizeof(m_szValueBuf));

	switch (m_nMenuStack[m_nMenuDepth])
	{
	case MenuHome:
	default:
		BuildHomePage();
		break;

	case MenuPerformance:
		BuildPerformancePage();
		break;

	case MenuStatus:
		BuildStatusPage();
		break;

	case MenuSaveSubmenu:
		BuildSaveSubmenuPage();
		break;

	case MenuDeleteConfirm:
		BuildDeleteConfirmPage();
		break;

	// Phase 3: Voices menus
	case MenuVoices:
		BuildVoicesPage();
		break;
	case MenuVoicesPitchBend:
		BuildVoicesPitchBendPage();
		break;
	case MenuVoicesPortamento:
		BuildVoicesPortamentoPage();
		break;
	case MenuVoicesNoteLimit:
		BuildVoicesNoteLimitPage();
		break;
	case MenuVoicesModulation:
		BuildVoicesModulationPage();
		break;
	case MenuVoicesMIDI:
		BuildVoicesMIDIPage();
		break;
	case MenuVoicesEQ:
		BuildVoicesEQPage();
		break;
	case MenuVoicesCompressor:
		BuildVoicesCompressorPage();
		break;
	case MenuVoicesEditVoice:
		BuildVoicesEditVoicePage();
		break;
	case MenuVoicesOperators:
		BuildVoicesOperatorsPage();
		break;

	// Phase 4: Effects menus
	case MenuEffects:
		BuildEffectsPage();
		break;
	case MenuEffectsCS2Input:
		BuildEffectsCS2InputPage();
		break;
	case MenuEffectsCS2Multitap:
		BuildEffectsCS2MultitapPage();
		break;
	case MenuEffectsCS2EarlyDiffusion:
		BuildEffectsCS2EarlyDiffusionPage();
		break;
	case MenuEffectsCS2LateDiffusion:
		BuildEffectsCS2LateDiffusionPage();
		break;
	case MenuEffectsCS2LateLines:
		BuildEffectsCS2LateLinesPage();
		break;
	case MenuEffectsCS2LowShelf:
		BuildEffectsCS2LowShelfPage();
		break;
	case MenuEffectsCS2HighShelf:
		BuildEffectsCS2HighShelfPage();
		break;
	case MenuEffectsCS2LowPass:
		BuildEffectsCS2LowPassPage();
		break;

	// Phase 5: Mixer
	case MenuMixer:
		BuildMixerPage();
		break;
	}
}

void CUI4Row::BuildHomePage()
{
	m_CurrentPage.pTitle = "Home";
	m_CurrentPage.nRowCount = 5;

	static const char *HomeLabels[] = {
		"Performance", "Voices", "Effects", "Mixer", "Status"
	};

	for (unsigned i = 0; i < 5; i++)
	{
		m_CurrentPage.Rows[i].Type = RowTypeMenuItem;
		m_CurrentPage.Rows[i].pLabel = HomeLabels[i];
		m_CurrentPage.Rows[i].pValue = "";
		m_CurrentPage.Rows[i].Action = ActionEnterSubmenu;
	}
}

void CUI4Row::BuildPerformancePage()
{
	m_CurrentPage.pTitle = "Performance";
	m_CurrentPage.nRowCount = 6;

	// Row 0: Bank
	{
		int nBank = m_pMiniDexed->GetPerformanceBank();
		std::string bankName = m_pMiniDexed->GetPerformanceConfig()->GetPerformanceBankName(nBank);
		snprintf(m_szValueBuf[0], VALUE_BUF_LEN, "%s", bankName.c_str());

		m_CurrentPage.Rows[0].Type = RowTypeProperty;
		m_CurrentPage.Rows[0].pLabel = "Bank";
		m_CurrentPage.Rows[0].pValue = m_szValueBuf[0];
		m_CurrentPage.Rows[0].Action = ActionNone;
	}

	// Row 1: Perf
	{
		int nPerfID = m_pMiniDexed->GetActualPerformanceID();
		// Safety: guard against race condition during bank switch
		if (m_pMiniDexed->IsValidPerformance(nPerfID))
		{
			std::string perfName = m_pMiniDexed->GetPerformanceName(nPerfID);
			snprintf(m_szValueBuf[1], VALUE_BUF_LEN, "%s", perfName.c_str());
		}
		else
		{
			snprintf(m_szValueBuf[1], VALUE_BUF_LEN, "%03d-...", nPerfID + 1);
		}

		m_CurrentPage.Rows[1].Type = RowTypeProperty;
		m_CurrentPage.Rows[1].pLabel = "Perf";
		m_CurrentPage.Rows[1].pValue = m_szValueBuf[1];
		m_CurrentPage.Rows[1].Action = ActionNone;
	}

	// Row 2: Save (submenu)
	{
		m_CurrentPage.Rows[2].Type = RowTypeMenuItem;
		m_CurrentPage.Rows[2].pLabel = "Save";
		m_CurrentPage.Rows[2].pValue = "";
		m_CurrentPage.Rows[2].Action = ActionEnterSubmenu;
	}

	// Row 3: Delete (submenu)
	{
		m_CurrentPage.Rows[3].Type = RowTypeMenuItem;
		m_CurrentPage.Rows[3].pLabel = "Delete";
		m_CurrentPage.Rows[3].pValue = "";
		m_CurrentPage.Rows[3].Action = ActionEnterSubmenu;
	}

	// Row 4: PCCH (Performance Select Channel)
	{
		int nPCCH = m_pMiniDexed->GetParameter(CMiniDexed::ParameterPerformanceSelectChannel);
		if (nPCCH >= (int)CMIDIDevice::Disabled)
			snprintf(m_szValueBuf[4], VALUE_BUF_LEN, "Off");
		else if (nPCCH == (int)CMIDIDevice::OmniMode)
			snprintf(m_szValueBuf[4], VALUE_BUF_LEN, "Omni");
		else
			snprintf(m_szValueBuf[4], VALUE_BUF_LEN, "%d", nPCCH + 1);

		m_CurrentPage.Rows[4].Type = RowTypeProperty;
		m_CurrentPage.Rows[4].pLabel = "PCCH";
		m_CurrentPage.Rows[4].pValue = m_szValueBuf[4];
		m_CurrentPage.Rows[4].Action = ActionNone;
	}

	// Row 5: Design Filter
	{
		int nFilter = m_pMiniDexed->GetParameter(CMiniDexed::ParameterSDFilter);
		int nTGs = m_pConfig->GetToneGenerators();
		SDFilter filter = SDFilter::to_filter(nFilter, nTGs);
		std::string filterStr = filter.to_string();
		snprintf(m_szValueBuf[5], VALUE_BUF_LEN, "%s", filterStr.c_str());

		m_CurrentPage.Rows[5].Type = RowTypeProperty;
		m_CurrentPage.Rows[5].pLabel = "Filter";
		m_CurrentPage.Rows[5].pValue = m_szValueBuf[5];
		m_CurrentPage.Rows[5].Action = ActionNone;
	}
}

void CUI4Row::BuildStatusPage()
{
	m_CurrentPage.pTitle = "Status";
	m_CurrentPage.nRowCount = 4;

	CStatus *pStatus = CStatus::Get();

	// Row 0: CPU Temp
	{
		snprintf(m_szValueBuf[0], VALUE_BUF_LEN, "%u/%u C",
			pStatus->nCPUTemp.load(), pStatus->nCPUMaxTemp);

		m_CurrentPage.Rows[0].Type = RowTypeReadOnly;
		m_CurrentPage.Rows[0].pLabel = "CPU Temp";
		m_CurrentPage.Rows[0].pValue = m_szValueBuf[0];
		m_CurrentPage.Rows[0].Action = ActionNone;
	}

	// Row 1: CPU Speed
	{
		snprintf(m_szValueBuf[1], VALUE_BUF_LEN, "%u/%u MHz",
			pStatus->nCPUClockRate.load() / 1000000,
			pStatus->nCPUMaxClockRate / 1000000);

		m_CurrentPage.Rows[1].Type = RowTypeReadOnly;
		m_CurrentPage.Rows[1].pLabel = "CPU Speed";
		m_CurrentPage.Rows[1].pValue = m_szValueBuf[1];
		m_CurrentPage.Rows[1].Action = ActionNone;
	}

	// Row 2: Net IP
	{
		CString IPString("-");
		const CIPAddress &IPAddr = m_pMiniDexed->GetNetworkIPAddress();
		if (IPAddr.IsSet())
		{
			IPAddr.Format(&IPString);
		}
		snprintf(m_szValueBuf[2], VALUE_BUF_LEN, "%s", (const char *)IPString);

		m_CurrentPage.Rows[2].Type = RowTypeReadOnly;
		m_CurrentPage.Rows[2].pLabel = "Net IP";
		m_CurrentPage.Rows[2].pValue = m_szValueBuf[2];
		m_CurrentPage.Rows[2].Action = ActionNone;
	}

	// Row 3: Version
	{
		snprintf(m_szValueBuf[3], VALUE_BUF_LEN, "%s", VERSION);

		m_CurrentPage.Rows[3].Type = RowTypeReadOnly;
		m_CurrentPage.Rows[3].pLabel = "Version";
		m_CurrentPage.Rows[3].pValue = m_szValueBuf[3];
		m_CurrentPage.Rows[3].Action = ActionNone;
	}
}

void CUI4Row::BuildSaveSubmenuPage()
{
	m_CurrentPage.pTitle = "Save";
	m_CurrentPage.nRowCount = 3;

	// Row 0: Overwrite
	{
		m_CurrentPage.Rows[0].Type = RowTypeAction;
		m_CurrentPage.Rows[0].pLabel = "Overwrite";
		m_CurrentPage.Rows[0].pValue = "";
		m_CurrentPage.Rows[0].Action = ActionCommand;
	}

	// Row 1: New (text input — stubbed until Phase 6)
	{
		m_CurrentPage.Rows[1].Type = RowTypeMenuItem;
		m_CurrentPage.Rows[1].pLabel = "New";
		m_CurrentPage.Rows[1].pValue = "";
		m_CurrentPage.Rows[1].Action = ActionEnterSubmenu;
	}

	// Row 2: Save as default
	{
		m_CurrentPage.Rows[2].Type = RowTypeAction;
		m_CurrentPage.Rows[2].pLabel = "Save as default";
		m_CurrentPage.Rows[2].pValue = "";
		m_CurrentPage.Rows[2].Action = ActionCommand;
	}
}

void CUI4Row::BuildDeleteConfirmPage()
{
	m_CurrentPage.pTitle = "Delete";
	m_CurrentPage.nRowCount = 3;

	// Row 0: "Delete 'PerfName'?"
	{
		int nPerfID = m_pMiniDexed->GetActualPerformanceID();
		std::string perfName = m_pMiniDexed->GetPerformanceName(nPerfID);
		snprintf(m_szValueBuf[0], VALUE_BUF_LEN, "\"%s\"?", perfName.c_str());

		m_CurrentPage.Rows[0].Type = RowTypeReadOnly;
		m_CurrentPage.Rows[0].pLabel = "Delete";
		m_CurrentPage.Rows[0].pValue = m_szValueBuf[0];
		m_CurrentPage.Rows[0].Action = ActionNone;
	}

	// Row 1: Confirm
	{
		m_CurrentPage.Rows[1].Type = RowTypeAction;
		m_CurrentPage.Rows[1].pLabel = "Confirm";
		m_CurrentPage.Rows[1].pValue = "";
		m_CurrentPage.Rows[1].Action = ActionCommand;
	}

	// Row 2: Cancel
	{
		m_CurrentPage.Rows[2].Type = RowTypeAction;
		m_CurrentPage.Rows[2].pLabel = "Cancel";
		m_CurrentPage.Rows[2].pValue = "";
		m_CurrentPage.Rows[2].Action = ActionCommand;
	}
}

// ============================================================================
// Phase 3: Voices Menu Builders
// ============================================================================

void CUI4Row::BuildVoicesPage()
{
	m_CurrentPage.pTitle = "Voices";

	unsigned nTG = m_nActiveTG;
	unsigned r = 0;

	// Row 0: TG selector
	snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "TG%u", nTG + 1);
	m_CurrentPage.Rows[r].Type = RowTypeProperty;
	m_CurrentPage.Rows[r].pLabel = "TG";
	m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
	m_CurrentPage.Rows[r].Action = ActionNone;
	r++;

	// Row 1: Bank
	{
		int nBank = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterVoiceBank, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nBank);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Bank";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 2: Voice
	{
		int nProg = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterProgram, nTG);
		std::string VoiceName = m_pMiniDexed->GetVoiceName(nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d-%s", nProg + 1, VoiceName.c_str());
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Voice";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 3: Volume
	{
		int nVal = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterVolume, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Volume";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 4: Pan
	{
		int nVal = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterPan, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Pan";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 5: FX1-Send
	{
		int nVal = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterFX1Send, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "FX1-Send";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 6: FX2-Send
	{
		int nVal = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterFX2Send, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "FX2-Send";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 7: Detune
	{
		int nVal = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterMasterTune, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Detune";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 8: Cutoff
	{
		int nVal = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterCutoff, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Cutoff";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 9: Resonance
	{
		int nVal = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterResonance, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Resonance";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 10: Pitch Bend (submenu)
	m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
	m_CurrentPage.Rows[r].pLabel = "Pitch Bend";
	m_CurrentPage.Rows[r].pValue = "";
	m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
	r++;

	// Row 11: Portamento (submenu)
	m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
	m_CurrentPage.Rows[r].pLabel = "Portamento";
	m_CurrentPage.Rows[r].pValue = "";
	m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
	r++;

	// Row 12: Note Limit (submenu)
	m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
	m_CurrentPage.Rows[r].pLabel = "Note Limit";
	m_CurrentPage.Rows[r].pValue = "";
	m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
	r++;

	// Row 13: Poly/Mono
	{
		int nVal = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterMonoMode, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", nVal ? "Mono" : "Poly");
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Poly/Mono";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 14: TG-Link
	{
		int nVal = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterTGLink, nTG);
		static const char *LinkNames[] = {"Off", "1", "2", "3", "4"};
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", LinkNames[nVal < 5 ? nVal : 0]);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "TG-Link";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 15: Modulation (submenu)
	m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
	m_CurrentPage.Rows[r].pLabel = "Modulation";
	m_CurrentPage.Rows[r].pValue = "";
	m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
	r++;

	// Row 16: MIDI (submenu)
	m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
	m_CurrentPage.Rows[r].pLabel = "MIDI";
	m_CurrentPage.Rows[r].pValue = "";
	m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
	r++;

	// Row 17: EQ (submenu)
	m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
	m_CurrentPage.Rows[r].pLabel = "EQ";
	m_CurrentPage.Rows[r].pValue = "";
	m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
	r++;

	// Row 18: Compressor (submenu)
	m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
	m_CurrentPage.Rows[r].pLabel = "Compressor";
	m_CurrentPage.Rows[r].pValue = "";
	m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
	r++;

	// Row 19: Edit Voice (submenu)
	m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
	m_CurrentPage.Rows[r].pLabel = "Edit Voice";
	m_CurrentPage.Rows[r].pValue = "";
	m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
	r++;

	m_CurrentPage.nRowCount = r;
}

void CUI4Row::BuildVoicesPitchBendPage()
{
	m_CurrentPage.pTitle = "Pitch Bend";
	m_CurrentPage.nRowCount = 2;
	unsigned nTG = m_nActiveTG;

	int nRange = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterPitchBendRange, nTG);
	snprintf(m_szValueBuf[0], VALUE_BUF_LEN, "%d", nRange);
	m_CurrentPage.Rows[0].Type = RowTypeProperty;
	m_CurrentPage.Rows[0].pLabel = "Bend Range";
	m_CurrentPage.Rows[0].pValue = m_szValueBuf[0];
	m_CurrentPage.Rows[0].Action = ActionNone;

	int nStep = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterPitchBendStep, nTG);
	snprintf(m_szValueBuf[1], VALUE_BUF_LEN, "%d", nStep);
	m_CurrentPage.Rows[1].Type = RowTypeProperty;
	m_CurrentPage.Rows[1].pLabel = "Bend Step";
	m_CurrentPage.Rows[1].pValue = m_szValueBuf[1];
	m_CurrentPage.Rows[1].Action = ActionNone;
}

void CUI4Row::BuildVoicesPortamentoPage()
{
	m_CurrentPage.pTitle = "Portamento";
	m_CurrentPage.nRowCount = 3;
	unsigned nTG = m_nActiveTG;

	int nMode = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterPortamentoMode, nTG);
	snprintf(m_szValueBuf[0], VALUE_BUF_LEN, "%s", nMode ? "Full" : "Fingered");
	m_CurrentPage.Rows[0].Type = RowTypeProperty;
	m_CurrentPage.Rows[0].pLabel = "Mode";
	m_CurrentPage.Rows[0].pValue = m_szValueBuf[0];
	m_CurrentPage.Rows[0].Action = ActionNone;

	int nGliss = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterPortamentoGlissando, nTG);
	snprintf(m_szValueBuf[1], VALUE_BUF_LEN, "%s", nGliss ? "On" : "Off");
	m_CurrentPage.Rows[1].Type = RowTypeProperty;
	m_CurrentPage.Rows[1].pLabel = "Glissando";
	m_CurrentPage.Rows[1].pValue = m_szValueBuf[1];
	m_CurrentPage.Rows[1].Action = ActionNone;

	int nTime = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterPortamentoTime, nTG);
	snprintf(m_szValueBuf[2], VALUE_BUF_LEN, "%d", nTime);
	m_CurrentPage.Rows[2].Type = RowTypeProperty;
	m_CurrentPage.Rows[2].pLabel = "Time";
	m_CurrentPage.Rows[2].pValue = m_szValueBuf[2];
	m_CurrentPage.Rows[2].Action = ActionNone;
}

void CUI4Row::BuildVoicesNoteLimitPage()
{
	m_CurrentPage.pTitle = "Note Limit";
	m_CurrentPage.nRowCount = 3;
	unsigned nTG = m_nActiveTG;

	static const char *NoteNames[] = {
		"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
	};

	int nLow = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterNoteLimitLow, nTG);
	snprintf(m_szValueBuf[0], VALUE_BUF_LEN, "%s%d", NoteNames[nLow % 12], (nLow / 12) - 2);
	m_CurrentPage.Rows[0].Type = RowTypeProperty;
	m_CurrentPage.Rows[0].pLabel = "Limit Low";
	m_CurrentPage.Rows[0].pValue = m_szValueBuf[0];
	m_CurrentPage.Rows[0].Action = ActionNone;

	int nHigh = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterNoteLimitHigh, nTG);
	snprintf(m_szValueBuf[1], VALUE_BUF_LEN, "%s%d", NoteNames[nHigh % 12], (nHigh / 12) - 2);
	m_CurrentPage.Rows[1].Type = RowTypeProperty;
	m_CurrentPage.Rows[1].pLabel = "Limit High";
	m_CurrentPage.Rows[1].pValue = m_szValueBuf[1];
	m_CurrentPage.Rows[1].Action = ActionNone;

	int nShift = m_pMiniDexed->GetTGParameter(CMiniDexed::TGParameterNoteShift, nTG);
	snprintf(m_szValueBuf[2], VALUE_BUF_LEN, "%d", nShift);
	m_CurrentPage.Rows[2].Type = RowTypeProperty;
	m_CurrentPage.Rows[2].pLabel = "Shift";
	m_CurrentPage.Rows[2].pValue = m_szValueBuf[2];
	m_CurrentPage.Rows[2].Action = ActionNone;
}

void CUI4Row::BuildVoicesModulationPage()
{
	m_CurrentPage.pTitle = "Modulation";
	m_CurrentPage.nRowCount = 5;
	unsigned nTG = m_nActiveTG;

	static const char *SourceNames[] = {"Mod.Wheel", "Foot Ctrl", "Breath", "Aftertouch"};

	// The TG parameter base for each source (MW=0, FC=1, BC=2, AT=3)
	// MW: TGParameterMWRange..TGParameterMWEGBias
	// FC: TGParameterFCRange..TGParameterFCEGBias
	// BC: TGParameterBCRange..TGParameterBCEGBias
	// AT: TGParameterATRange..TGParameterATEGBias
	unsigned nBase = CMiniDexed::TGParameterMWRange + m_nActiveModSource * 4;

	// Row 0: Source selector
	snprintf(m_szValueBuf[0], VALUE_BUF_LEN, "%s", SourceNames[m_nActiveModSource]);
	m_CurrentPage.Rows[0].Type = RowTypeProperty;
	m_CurrentPage.Rows[0].pLabel = "Source";
	m_CurrentPage.Rows[0].pValue = m_szValueBuf[0];
	m_CurrentPage.Rows[0].Action = ActionNone;

	// Row 1: Range
	int nRange = m_pMiniDexed->GetTGParameter((CMiniDexed::TTGParameter)(nBase + 0), nTG);
	snprintf(m_szValueBuf[1], VALUE_BUF_LEN, "%d", nRange);
	m_CurrentPage.Rows[1].Type = RowTypeProperty;
	m_CurrentPage.Rows[1].pLabel = "Range";
	m_CurrentPage.Rows[1].pValue = m_szValueBuf[1];
	m_CurrentPage.Rows[1].Action = ActionNone;

	// Row 2: Pitch
	int nPitch = m_pMiniDexed->GetTGParameter((CMiniDexed::TTGParameter)(nBase + 1), nTG);
	snprintf(m_szValueBuf[2], VALUE_BUF_LEN, "%s", nPitch ? "On" : "Off");
	m_CurrentPage.Rows[2].Type = RowTypeProperty;
	m_CurrentPage.Rows[2].pLabel = "Pitch";
	m_CurrentPage.Rows[2].pValue = m_szValueBuf[2];
	m_CurrentPage.Rows[2].Action = ActionNone;

	// Row 3: Amplitude
	int nAmp = m_pMiniDexed->GetTGParameter((CMiniDexed::TTGParameter)(nBase + 2), nTG);
	snprintf(m_szValueBuf[3], VALUE_BUF_LEN, "%s", nAmp ? "On" : "Off");
	m_CurrentPage.Rows[3].Type = RowTypeProperty;
	m_CurrentPage.Rows[3].pLabel = "Amplitude";
	m_CurrentPage.Rows[3].pValue = m_szValueBuf[3];
	m_CurrentPage.Rows[3].Action = ActionNone;

	// Row 4: EG Bias
	int nEG = m_pMiniDexed->GetTGParameter((CMiniDexed::TTGParameter)(nBase + 3), nTG);
	snprintf(m_szValueBuf[4], VALUE_BUF_LEN, "%s", nEG ? "On" : "Off");
	m_CurrentPage.Rows[4].Type = RowTypeProperty;
	m_CurrentPage.Rows[4].pLabel = "EG Bias";
	m_CurrentPage.Rows[4].pValue = m_szValueBuf[4];
	m_CurrentPage.Rows[4].Action = ActionNone;
}

void CUI4Row::BuildVoicesMIDIPage()
{
	m_CurrentPage.pTitle = "MIDI";
	m_CurrentPage.nRowCount = 7;
	unsigned nTG = m_nActiveTG;

	struct { const char *label; CMiniDexed::TTGParameter param; } items[] = {
		{"Channel",       CMiniDexed::TGParameterMIDIChannel},
		{"SysEx Ch",      CMiniDexed::TGParameterSysExChannel},
		{"SysEx Enable",  CMiniDexed::TGParameterSysExEnable},
		{"Sustain Rx",    CMiniDexed::TGParameterMIDIRxSustain},
		{"Portamento Rx", CMiniDexed::TGParameterMIDIRxPortamento},
		{"Sostenuto Rx",  CMiniDexed::TGParameterMIDIRxSostenuto},
		{"Hold2 Rx",      CMiniDexed::TGParameterMIDIRxHold2},
	};

	for (unsigned i = 0; i < 7; i++)
	{
		int nVal = m_pMiniDexed->GetTGParameter(items[i].param, nTG);

		// Channel params show 1-based value, toggle params show On/Off
		if (items[i].param == CMiniDexed::TGParameterMIDIChannel ||
		    items[i].param == CMiniDexed::TGParameterSysExChannel)
		{
			snprintf(m_szValueBuf[i], VALUE_BUF_LEN, "%d", nVal + 1);
		}
		else if (items[i].param == CMiniDexed::TGParameterSysExEnable ||
		         items[i].param == CMiniDexed::TGParameterMIDIRxSustain ||
		         items[i].param == CMiniDexed::TGParameterMIDIRxPortamento ||
		         items[i].param == CMiniDexed::TGParameterMIDIRxSostenuto ||
		         items[i].param == CMiniDexed::TGParameterMIDIRxHold2)
		{
			snprintf(m_szValueBuf[i], VALUE_BUF_LEN, "%s", nVal ? "On" : "Off");
		}
		else
		{
			snprintf(m_szValueBuf[i], VALUE_BUF_LEN, "%d", nVal);
		}

		m_CurrentPage.Rows[i].Type = RowTypeProperty;
		m_CurrentPage.Rows[i].pLabel = items[i].label;
		m_CurrentPage.Rows[i].pValue = m_szValueBuf[i];
		m_CurrentPage.Rows[i].Action = ActionNone;
	}
}

void CUI4Row::BuildVoicesEQPage()
{
	m_CurrentPage.pTitle = "EQ";
	m_CurrentPage.nRowCount = 8;
	unsigned nTG = m_nActiveTG;

	struct { const char *label; CMiniDexed::TTGParameter param; } items[] = {
		{"Low Level",     CMiniDexed::TGParameterEQLow},
		{"Mid Level",     CMiniDexed::TGParameterEQMid},
		{"High Level",    CMiniDexed::TGParameterEQHigh},
		{"Gain",          CMiniDexed::TGParameterEQGain},
		{"Low-Mid Freq",  CMiniDexed::TGParameterEQLowMidFreq},
		{"Mid-High Freq", CMiniDexed::TGParameterEQMidHighFreq},
		{"Pre Lowcut",    CMiniDexed::TGParameterEQPreLowcut},
		{"Pre Highcut",   CMiniDexed::TGParameterEQPreHighcut},
	};

	for (unsigned i = 0; i < 8; i++)
	{
		int nVal = m_pMiniDexed->GetTGParameter(items[i].param, nTG);
		snprintf(m_szValueBuf[i], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[i].Type = RowTypeProperty;
		m_CurrentPage.Rows[i].pLabel = items[i].label;
		m_CurrentPage.Rows[i].pValue = m_szValueBuf[i];
		m_CurrentPage.Rows[i].Action = ActionNone;
	}
}

void CUI4Row::BuildVoicesCompressorPage()
{
	m_CurrentPage.pTitle = "Compressor";
	m_CurrentPage.nRowCount = 7;
	unsigned nTG = m_nActiveTG;

	struct { const char *label; CMiniDexed::TTGParameter param; bool bOnOff; } items[] = {
		{"Enable",      CMiniDexed::TGParameterCompressorEnable, true},
		{"Pre Gain",    CMiniDexed::TGParameterCompressorPreGain, false},
		{"Threshold",   CMiniDexed::TGParameterCompressorThresh, false},
		{"Ratio",       CMiniDexed::TGParameterCompressorRatio, false},
		{"Attack",      CMiniDexed::TGParameterCompressorAttack, false},
		{"Release",     CMiniDexed::TGParameterCompressorRelease, false},
		{"Makeup Gain", CMiniDexed::TGParameterCompressorMakeupGain, false},
	};

	for (unsigned i = 0; i < 7; i++)
	{
		int nVal = m_pMiniDexed->GetTGParameter(items[i].param, nTG);
		if (items[i].bOnOff)
			snprintf(m_szValueBuf[i], VALUE_BUF_LEN, "%s", nVal ? "On" : "Off");
		else
			snprintf(m_szValueBuf[i], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[i].Type = RowTypeProperty;
		m_CurrentPage.Rows[i].pLabel = items[i].label;
		m_CurrentPage.Rows[i].pValue = m_szValueBuf[i];
		m_CurrentPage.Rows[i].Action = ActionNone;
	}
}

void CUI4Row::BuildVoicesEditVoicePage()
{
	m_CurrentPage.pTitle = "Edit Voice";
	unsigned nTG = m_nActiveTG;
	unsigned r = 0;

	// Row 0: Algorithm (1-32)
	{
		int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_ALGORITHM, 6, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal + 1);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Algorithm";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 1: Feedback (0-7)
	{
		int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_FEEDBACK, 6, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Feedback";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 2: Operators (submenu)
	m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
	m_CurrentPage.Rows[r].pLabel = "Operators";
	m_CurrentPage.Rows[r].pValue = "";
	m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
	r++;

	// Row 3-10: Pitch EG Rates and Levels
	static const char *PEGLabels[] = {
		"P EG Rate 1", "P EG Rate 2", "P EG Rate 3", "P EG Rate 4",
		"P EG Level 1", "P EG Level 2", "P EG Level 3", "P EG Level 4",
	};
	static const int PEGParams[] = {
		DEXED_PITCH_EG_R1, DEXED_PITCH_EG_R2, DEXED_PITCH_EG_R3, DEXED_PITCH_EG_R4,
		DEXED_PITCH_EG_L1, DEXED_PITCH_EG_L2, DEXED_PITCH_EG_L3, DEXED_PITCH_EG_L4,
	};
	for (unsigned i = 0; i < 8; i++)
	{
		int nVal = m_pMiniDexed->GetVoiceParameter(PEGParams[i], 6, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = PEGLabels[i];
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 11: Osc Key Sync
	{
		int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_OSC_KEY_SYNC, 6, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", nVal ? "On" : "Off");
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Osc Key Sync";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 12-17: LFO params
	static const char *LFOLabels[] = {
		"LFO Speed", "LFO Delay", "LFO PMD", "LFO AMD", "LFO Sync", "LFO Wave",
	};
	static const int LFOParams[] = {
		DEXED_LFO_SPEED, DEXED_LFO_DELAY, DEXED_LFO_PITCH_MOD_DEP,
		DEXED_LFO_AMP_MOD_DEP, DEXED_LFO_SYNC, DEXED_LFO_WAVE,
	};
	static const char *LFOWaveNames[] = {
		"Triangle", "Saw Down", "Saw Up", "Square", "Sine", "S&H"
	};
	for (unsigned i = 0; i < 6; i++)
	{
		int nVal = m_pMiniDexed->GetVoiceParameter(LFOParams[i], 6, nTG);
		if (LFOParams[i] == DEXED_LFO_SYNC)
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", nVal ? "On" : "Off");
		else if (LFOParams[i] == DEXED_LFO_WAVE)
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", LFOWaveNames[nVal < 6 ? nVal : 0]);
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = LFOLabels[i];
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 18: P Mod Sens.
	{
		int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_LFO_PITCH_MOD_SENS, 6, nTG);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "P Mod Sens.";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 19: Transpose (0-48, displayed as note)
	{
		int nVal = m_pMiniDexed->GetVoiceParameter(DEXED_TRANSPOSE, 6, nTG);
		static const char *NoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		int nNote = nVal % 12;
		int nOctave = (nVal / 12) - 2;
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s%d", NoteNames[nNote], nOctave);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Transpose";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	m_CurrentPage.nRowCount = r;
}

void CUI4Row::BuildVoicesOperatorsPage()
{
	m_CurrentPage.pTitle = "Operators";
	unsigned nTG = m_nActiveTG;
	unsigned nOP = m_nActiveOP;
	unsigned r = 0;

	// Row 0: OP selector
	snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "OP%u", nOP + 1);
	m_CurrentPage.Rows[r].Type = RowTypeProperty;
	m_CurrentPage.Rows[r].pLabel = "OP";
	m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
	m_CurrentPage.Rows[r].Action = ActionNone;
	r++;

	// Operator parameter items
	struct { const char *label; int param; bool bOnOff; } OPItems[] = {
		{"Output Level",  DEXED_OP_OUTPUT_LEV, false},
		{"Freq Coarse",   DEXED_OP_FREQ_COARSE, false},
		{"Freq Fine",     DEXED_OP_FREQ_FINE, false},
		{"Osc Detune",    DEXED_OP_OSC_DETUNE, false},
		{"Osc Mode",      DEXED_OP_OSC_MODE, false},
		{"EG Rate 1",     DEXED_OP_EG_R1, false},
		{"EG Rate 2",     DEXED_OP_EG_R2, false},
		{"EG Rate 3",     DEXED_OP_EG_R3, false},
		{"EG Rate 4",     DEXED_OP_EG_R4, false},
		{"EG Level 1",    DEXED_OP_EG_L1, false},
		{"EG Level 2",    DEXED_OP_EG_L2, false},
		{"EG Level 3",    DEXED_OP_EG_L3, false},
		{"EG Level 4",    DEXED_OP_EG_L4, false},
		{"Break Point",   DEXED_OP_LEV_SCL_BRK_PT, false},
		{"L Key Depth",   DEXED_OP_SCL_LEFT_DEPTH, false},
		{"R Key Depth",   DEXED_OP_SCL_RGHT_DEPTH, false},
		{"L Key Scale",   DEXED_OP_SCL_LEFT_CURVE, false},
		{"R Key Scale",   DEXED_OP_SCL_RGHT_CURVE, false},
		{"Rate Scaling",  DEXED_OP_OSC_RATE_SCALE, false},
		{"A Mod Sens.",   DEXED_OP_AMP_MOD_SENS, false},
		{"K Vel. Sens.",  DEXED_OP_KEY_VEL_SENS, false},
		{"Enable",        DEXED_OP_ENABLE, true},
	};

	for (unsigned i = 0; i < 22; i++)
	{
		int nVal = m_pMiniDexed->GetVoiceParameter(OPItems[i].param, nOP, nTG);

		if (OPItems[i].bOnOff)
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", nVal ? "On" : "Off");
		else if (OPItems[i].param == DEXED_OP_OSC_MODE)
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", nVal ? "Fixed" : "Ratio");
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = OPItems[i].label;
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	m_CurrentPage.nRowCount = r;
}

// ============================================================================
// Phase 4: Effects Menu Builders
// ============================================================================

int CUI4Row::GetActiveFxChainIndex() const
{
	if (m_nActiveFxBus >= (unsigned)CConfig::BusFXChains)
		return CConfig::MasterFX;
	return (int)(m_nActiveFxBus + m_nActiveBus * CConfig::BusFXChains);
}

void CUI4Row::BuildEffectsPage()
{
	m_CurrentPage.pTitle = "Effects";
	unsigned r = 0;

	int nFX = GetActiveFxChainIndex();
	bool bIsMaster = (m_nActiveFxBus >= (unsigned)CConfig::BusFXChains);

	// Row 0: Fx bus selector (always first, so rows don't shift)
	{
		static const char *BusNames[] = {"SendFX1", "SendFX2", "MasterFX"};
		unsigned nameIdx = m_nActiveFxBus;
		if (nameIdx > 2) nameIdx = 2;
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Fx bus";
		m_CurrentPage.Rows[r].pValue = BusNames[nameIdx];
		int nBypass = m_pMiniDexed->GetFXParameter(FX::Bypass, nFX);
		m_CurrentPage.Rows[r].Action = ActionToggle;
		r++;
	}

	// Row 1: Dry Level (only if SendFX, hidden for MasterFX)
	if (!bIsMaster)
	{
		int nVal = m_pMiniDexed->GetBusParameter(Bus::MixerDryLevel, m_nActiveBus);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Dry Level";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Fx slot selector row
	{
		static const char *SlotNames[] = {"1", "2", "3"};
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Fx slot";
		m_CurrentPage.Rows[r].pValue = SlotNames[m_nActiveFxSlot];

		FX::Parameter slotParam = (FX::Parameter)(FX::Parameter::Slot0 + m_nActiveFxSlot);
		int nAlg = m_pMiniDexed->GetFXParameter(slotParam, nFX);
		if (nAlg > 0 && nAlg < FX::effects_num)
		{
			int bypassParam = FX::s_effects[nAlg].MaxID;
			int nBp = m_pMiniDexed->GetFXParameter((FX::Parameter)bypassParam, nFX);
			m_CurrentPage.Rows[r].Action = ActionToggle;
		}
		else
		{
			m_CurrentPage.Rows[r].Action = ActionNone;
		}
		r++;
	}

	// Fx algorithm selector row
	{
		FX::Parameter slotParam = (FX::Parameter)(FX::Parameter::Slot0 + m_nActiveFxSlot);
		int nAlg = m_pMiniDexed->GetFXParameter(slotParam, nFX);
		const char *algName = "None";
		if (nAlg >= 0 && nAlg < FX::effects_num)
			algName = FX::s_effects[nAlg].Name;
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Fx";
		m_CurrentPage.Rows[r].pValue = algName;
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Dynamic algorithm-specific params
	{
		FX::Parameter slotParam = (FX::Parameter)(FX::Parameter::Slot0 + m_nActiveFxSlot);
		int nAlg = m_pMiniDexed->GetFXParameter(slotParam, nFX);
		if (nAlg > 0 && nAlg < FX::effects_num)
		{
			int minID = FX::s_effects[nAlg].MinID;
			int maxID = FX::s_effects[nAlg].MaxID;
			const char *algPrefix = FX::s_effects[nAlg].Name;
			unsigned nPrefixLen = algPrefix ? strlen(algPrefix) : 0;

			bool bIsCS2 = (nAlg == 9); // CloudSeed2 is index 9 in s_effects

			if (bIsCS2)
			{
				// CloudSeed2 top-level params
				static const struct { const char *label; FX::Parameter param; } CS2TopLevel[] = {
					{"Load Preset",  FX::CloudSeed2Preset},
					{"Dry Out",      FX::CloudSeed2DryOut},
					{"Early Out",    FX::CloudSeed2EarlyOut},
					{"Late Out",     FX::CloudSeed2LateOut},
					{"Early FB",     FX::CloudSeed2EarlyDiffuseFeedback},
					{"Late FB",      FX::CloudSeed2LateDiffuseFeedback},
					{"Tap Decay",    FX::CloudSeed2TapDecay},
					{"Late Decay",   FX::CloudSeed2LateLineDecay},
					{"Late Lines",   FX::CloudSeed2LateLineCount},
				};

				for (unsigned i = 0; i < sizeof(CS2TopLevel)/sizeof(CS2TopLevel[0]) && r < MAX_ROWS; i++)
				{
					int nVal = m_pMiniDexed->GetFXParameter(CS2TopLevel[i].param, nFX);
					FX::ParameterType &pt = FX::s_Parameter[CS2TopLevel[i].param];
					if (pt.ToString)
					{
						std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
						snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
					}
					else
						snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

					m_CurrentPage.Rows[r].Type = RowTypeProperty;
					m_CurrentPage.Rows[r].pLabel = CS2TopLevel[i].label;
					m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
					m_CurrentPage.Rows[r].Action = ActionNone;
					r++;
				}

				// CloudSeed2 sub-menu entries
				static const char *CS2SubMenuLabels[] = {
					"Input", "Multitap Delay", "Early Diffusion", "Late Diffusion",
					"Late Lines", "Low Shelf", "High Shelf", "Low Pass"
				};
				for (unsigned i = 0; i < 8 && r < MAX_ROWS; i++)
				{
					m_CurrentPage.Rows[r].Type = RowTypeMenuItem;
					m_CurrentPage.Rows[r].pLabel = CS2SubMenuLabels[i];
					m_CurrentPage.Rows[r].pValue = "";
					m_CurrentPage.Rows[r].Action = ActionEnterSubmenu;
					r++;
				}

				// CloudSeed2 bypass
				{
					int nBp = m_pMiniDexed->GetFXParameter(FX::CloudSeed2Bypass, nFX);
					snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", nBp ? "On" : "Off");
					m_CurrentPage.Rows[r].Type = RowTypeProperty;
					m_CurrentPage.Rows[r].pLabel = "Bypass";
					m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
					m_CurrentPage.Rows[r].Action = ActionNone;
					r++;
				}
			}
			else
			{
				// Regular algorithm: iterate all params from MinID to MaxID
				for (int pid = minID; pid <= maxID && r < MAX_ROWS; pid++)
				{
					FX::Parameter fp = (FX::Parameter)pid;
					FX::ParameterType &pt = FX::s_Parameter[fp];
					int nVal = m_pMiniDexed->GetFXParameter(fp, nFX);

					if (pt.ToString)
					{
						std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
						snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
					}
					else
						snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

					// Strip algorithm prefix from parameter name
					// e.g. "YKChorusMix" -> "Mix", "ZynDistMix" -> "Mix"
					const char *pLabel = pt.Name ? pt.Name : "???";
					if (nPrefixLen > 0 && strncmp(pLabel, algPrefix, nPrefixLen) == 0)
						pLabel += nPrefixLen;

					m_CurrentPage.Rows[r].Type = RowTypeProperty;
					m_CurrentPage.Rows[r].pLabel = pLabel;
					m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
					m_CurrentPage.Rows[r].Action = ActionNone;
					r++;
				}
			}
		}
	}

	m_CurrentPage.nRowCount = r;
}

// --- CloudSeed2 Sub-page Builders ---

void CUI4Row::BuildEffectsCS2InputPage()
{
	m_CurrentPage.pTitle = "CS2 Input";
	unsigned r = 0;
	int nFX = GetActiveFxChainIndex();

	static const struct { const char *label; FX::Parameter param; } items[] = {
		{"Interpolation",  FX::CloudSeed2Interpolation},
		{"L/R Input Mix",  FX::CloudSeed2InputMix},
		{"High Cut Ena.",  FX::CloudSeed2HighCutEnabled},
		{"High Cut",       FX::CloudSeed2HighCut},
		{"Low Cut Ena.",   FX::CloudSeed2LowCutEnabled},
		{"Low Cut",        FX::CloudSeed2LowCut},
	};

	for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]) && r < MAX_ROWS; i++)
	{
		int nVal = m_pMiniDexed->GetFXParameter(items[i].param, nFX);
		FX::ParameterType &pt = FX::s_Parameter[items[i].param];
		if (pt.ToString)
		{
			std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
		}
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = items[i].label;
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}
	m_CurrentPage.nRowCount = r;
}

void CUI4Row::BuildEffectsCS2MultitapPage()
{
	m_CurrentPage.pTitle = "CS2 Multitap";
	unsigned r = 0;
	int nFX = GetActiveFxChainIndex();

	static const struct { const char *label; FX::Parameter param; } items[] = {
		{"Enabled",    FX::CloudSeed2TapEnabled},
		{"Count",      FX::CloudSeed2TapCount},
		{"Decay",      FX::CloudSeed2TapDecay},
		{"Predelay",   FX::CloudSeed2TapPredelay},
		{"Length",     FX::CloudSeed2TapLength},
	};

	for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]) && r < MAX_ROWS; i++)
	{
		int nVal = m_pMiniDexed->GetFXParameter(items[i].param, nFX);
		FX::ParameterType &pt = FX::s_Parameter[items[i].param];
		if (pt.ToString)
		{
			std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
		}
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = items[i].label;
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}
	m_CurrentPage.nRowCount = r;
}

void CUI4Row::BuildEffectsCS2EarlyDiffusionPage()
{
	m_CurrentPage.pTitle = "CS2 Early Diffusion";
	unsigned r = 0;
	int nFX = GetActiveFxChainIndex();

	static const struct { const char *label; FX::Parameter param; } items[] = {
		{"Enabled",      FX::CloudSeed2EarlyDiffuseEnabled},
		{"Stage Count",  FX::CloudSeed2EarlyDiffuseCount},
		{"Delay",        FX::CloudSeed2EarlyDiffuseDelay},
		{"Feedback",     FX::CloudSeed2EarlyDiffuseFeedback},
		{"Mod Amount",   FX::CloudSeed2EarlyDiffuseModAmount},
		{"Mod Rate",     FX::CloudSeed2EarlyDiffuseModRate},
	};

	for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]) && r < MAX_ROWS; i++)
	{
		int nVal = m_pMiniDexed->GetFXParameter(items[i].param, nFX);
		FX::ParameterType &pt = FX::s_Parameter[items[i].param];
		if (pt.ToString)
		{
			std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
		}
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = items[i].label;
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}
	m_CurrentPage.nRowCount = r;
}

void CUI4Row::BuildEffectsCS2LateDiffusionPage()
{
	m_CurrentPage.pTitle = "CS2 Late Diffusion";
	unsigned r = 0;
	int nFX = GetActiveFxChainIndex();

	static const struct { const char *label; FX::Parameter param; } items[] = {
		{"Enabled",      FX::CloudSeed2LateDiffuseEnabled},
		{"Stage Count",  FX::CloudSeed2LateDiffuseCount},
		{"Delay",        FX::CloudSeed2LateDiffuseDelay},
		{"Feedback",     FX::CloudSeed2LateDiffuseFeedback},
		{"Mod Amount",   FX::CloudSeed2LateDiffuseModAmount},
		{"Mod Rate",     FX::CloudSeed2LateDiffuseModRate},
	};

	for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]) && r < MAX_ROWS; i++)
	{
		int nVal = m_pMiniDexed->GetFXParameter(items[i].param, nFX);
		FX::ParameterType &pt = FX::s_Parameter[items[i].param];
		if (pt.ToString)
		{
			std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
		}
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = items[i].label;
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}
	m_CurrentPage.nRowCount = r;
}

void CUI4Row::BuildEffectsCS2LateLinesPage()
{
	m_CurrentPage.pTitle = "CS2 Late Lines";
	unsigned r = 0;
	int nFX = GetActiveFxChainIndex();

	static const struct { const char *label; FX::Parameter param; } items[] = {
		{"Mode",       FX::CloudSeed2LateMode},
		{"Count",      FX::CloudSeed2LateLineCount},
		{"Size",       FX::CloudSeed2LateLineSize},
		{"Decay",      FX::CloudSeed2LateLineDecay},
		{"Mod Amt",    FX::CloudSeed2LateLineModAmount},
		{"Mod Rate",   FX::CloudSeed2LateLineModRate},
	};

	for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]) && r < MAX_ROWS; i++)
	{
		int nVal = m_pMiniDexed->GetFXParameter(items[i].param, nFX);
		FX::ParameterType &pt = FX::s_Parameter[items[i].param];
		if (pt.ToString)
		{
			std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
		}
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = items[i].label;
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}
	m_CurrentPage.nRowCount = r;
}

void CUI4Row::BuildEffectsCS2LowShelfPage()
{
	m_CurrentPage.pTitle = "CS2 Low Shelf";
	unsigned r = 0;
	int nFX = GetActiveFxChainIndex();

	static const struct { const char *label; FX::Parameter param; } items[] = {
		{"Enable",  FX::CloudSeed2EqLowShelfEnabled},
		{"Freq",    FX::CloudSeed2EqLowFreq},
		{"Gain",    FX::CloudSeed2EqLowGain},
	};

	for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]) && r < MAX_ROWS; i++)
	{
		int nVal = m_pMiniDexed->GetFXParameter(items[i].param, nFX);
		FX::ParameterType &pt = FX::s_Parameter[items[i].param];
		if (pt.ToString)
		{
			std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
		}
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = items[i].label;
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}
	m_CurrentPage.nRowCount = r;
}

void CUI4Row::BuildEffectsCS2HighShelfPage()
{
	m_CurrentPage.pTitle = "CS2 High Shelf";
	unsigned r = 0;
	int nFX = GetActiveFxChainIndex();

	static const struct { const char *label; FX::Parameter param; } items[] = {
		{"Enable",  FX::CloudSeed2EqHighShelfEnabled},
		{"Freq",    FX::CloudSeed2EqHighFreq},
		{"Gain",    FX::CloudSeed2EqHighGain},
	};

	for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]) && r < MAX_ROWS; i++)
	{
		int nVal = m_pMiniDexed->GetFXParameter(items[i].param, nFX);
		FX::ParameterType &pt = FX::s_Parameter[items[i].param];
		if (pt.ToString)
		{
			std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
		}
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = items[i].label;
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}
	m_CurrentPage.nRowCount = r;
}

void CUI4Row::BuildEffectsCS2LowPassPage()
{
	m_CurrentPage.pTitle = "CS2 Low Pass";
	unsigned r = 0;
	int nFX = GetActiveFxChainIndex();

	static const struct { const char *label; FX::Parameter param; } items[] = {
		{"Enable",  FX::CloudSeed2EqLowpassEnabled},
		{"Cutoff",  FX::CloudSeed2EqCutoff},
	};

	for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]) && r < MAX_ROWS; i++)
	{
		int nVal = m_pMiniDexed->GetFXParameter(items[i].param, nFX);
		FX::ParameterType &pt = FX::s_Parameter[items[i].param];
		if (pt.ToString)
		{
			std::string s = pt.ToString(nVal, VALUE_BUF_LEN - 1);
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%s", s.c_str());
		}
		else
			snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);

		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = items[i].label;
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}
	m_CurrentPage.nRowCount = r;
}

// ============================================================================
// Phase 5: Mixer Builder
// ============================================================================

void CUI4Row::BuildMixerPage()
{
	m_CurrentPage.pTitle = "Mixer";
	unsigned r = 0;

	// Row 0: Master Volume with mute toggle
	{
		int nVal = m_pMiniDexed->GetParameter(CMiniDexed::ParameterMasterVolume);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Master Vol";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionToggle;
		r++;
	}

	// Row 1: Dry Level (Bus 0)
	{
		int nVal = m_pMiniDexed->GetBusParameter(Bus::MixerDryLevel, 0);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Dry Level";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 2: FX1 Return (SendFX chain 0)
	{
		int nVal = m_pMiniDexed->GetFXParameter(FX::ReturnLevel, 0);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "FX1 Return";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 3: FX2 Return (SendFX chain 1)
	{
		int nVal = m_pMiniDexed->GetFXParameter(FX::ReturnLevel, 1);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "FX2 Return";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	// Row 4: Return Level (Bus 0)
	{
		int nVal = m_pMiniDexed->GetBusParameter(Bus::ReturnLevel, 0);
		snprintf(m_szValueBuf[r], VALUE_BUF_LEN, "%d", nVal);
		m_CurrentPage.Rows[r].Type = RowTypeProperty;
		m_CurrentPage.Rows[r].pLabel = "Return";
		m_CurrentPage.Rows[r].pValue = m_szValueBuf[r];
		m_CurrentPage.Rows[r].Action = ActionNone;
		r++;
	}

	m_CurrentPage.nRowCount = r;
}

// ============================================================================
// Rendering (using u8g2 for pixel-precise layout)
// ============================================================================

void CUI4Row::RenderScrollbar()
{
	unsigned nTotal = m_CurrentPage.nRowCount;
	unsigned nBarH, nBarY;

	if (nTotal <= SCREEN_ROWS)
	{
		nBarH = SCREEN_HEIGHT_PX;
		nBarY = 0;
	}
	else
	{
		nBarH = (SCREEN_ROWS * SCREEN_HEIGHT_PX) / nTotal;
		if (nBarH < SCROLLBAR_MIN_HEIGHT)
			nBarH = SCROLLBAR_MIN_HEIGHT;

		unsigned nMaxScroll = nTotal - SCREEN_ROWS;
		if (nMaxScroll > 0)
			nBarY = (m_nScrollIndex * (SCREEN_HEIGHT_PX - nBarH)) / nMaxScroll;
		else
			nBarY = 0;
	}

	u8g2_DrawBox(&m_u8g2, 0, (u8g2_uint_t)nBarY,
	             SCROLLBAR_WIDTH_PX, (u8g2_uint_t)nBarH);
}

void CUI4Row::RenderRow(unsigned nScreenRow, unsigned nItemIndex)
{
	if (nScreenRow >= SCREEN_ROWS)
		return;

	if (nItemIndex >= m_CurrentPage.nRowCount)
		return; // blank row — already cleared by u8g2_ClearBuffer

	const TMenuRow &row = m_CurrentPage.Rows[nItemIndex];

	// --- Build text ---
	char szText[22];
	if (row.pLabel && row.pLabel[0])
	{
		if (row.pValue && row.pValue[0])
			snprintf(szText, sizeof(szText), "%s: %s", row.pLabel, row.pValue);
		else
			snprintf(szText, sizeof(szText), "%s", row.pLabel);
	}
	else
	{
		szText[0] = '\0';
	}

	// Draw text at the correct baseline for this row
	u8g2_SetFont(&m_u8g2, u8g2_font_t0_13b_tr);
	u8g2_SetFontMode(&m_u8g2, 1);  // transparent mode
	u8g2_SetDrawColor(&m_u8g2, 1); // white
	u8g2_DrawStr(&m_u8g2, TEXT_X, TEXT_BASELINE[nScreenRow], szText);

	// --- Draw action indicator (▶ in white box) ---
	if (row.Action != ActionNone)
	{
		// White background box
		u8g2_SetDrawColor(&m_u8g2, 1);
		u8g2_DrawBox(&m_u8g2,
		             ACTION_BOX_X, ACTION_BOX_Y[nScreenRow],
		             ACTION_BOX_W, ACTION_BOX_H);

		// ▶ icon in XOR mode (appears dark on white background)
		u8g2_SetDrawColor(&m_u8g2, 2); // XOR
		u8g2_SetBitmapMode(&m_u8g2, 1); // transparent
		u8g2_DrawXBM(&m_u8g2,
		             ACTION_ICON_X, ACTION_ICON_Y[nScreenRow],
		             ACTION_ICON_W, ACTION_ICON_H,
		             s_ActionIcon);

		// Reset draw color
		u8g2_SetDrawColor(&m_u8g2, 1);
	}
}

void CUI4Row::Render(CSSD1306Device *pDisplay)
{
	if (!m_bDirty || !pDisplay)
		return;

	// 1. Clear u8g2 buffer
	u8g2_ClearBuffer(&m_u8g2);

	// 2. Set rendering modes
	u8g2_SetFontMode(&m_u8g2, 1);   // transparent font background
	u8g2_SetBitmapMode(&m_u8g2, 1); // transparent bitmaps

	// 3. Draw scrollbar (left zone)
	RenderScrollbar();

	// 4. Draw 4 visible rows (text + action indicators)
	for (unsigned row = 0; row < SCREEN_ROWS; row++)
	{
		RenderRow(row, m_nScrollIndex + row);
	}

	// 5. Clear the SSD1306 framebuffer first (ensures diff-check triggers
	//    even if the text LCD path wrote to both buffers previously)
	pDisplay->Clear();

	// 6. Copy u8g2 buffer → SSD1306 framebuffer
	uint8_t *pU8g2Buf = u8g2_GetBufferPtr(&m_u8g2);
	uint8_t *pDisplayBuf = pDisplay->GetFrameBuffer();
	unsigned nBufSize = pDisplay->GetFrameBufferSize();

	if (pU8g2Buf && pDisplayBuf && nBufSize > 0)
	{
		memcpy(pDisplayBuf, pU8g2Buf, nBufSize);
	}

	// 7. Send to display hardware
	pDisplay->Flip();

	m_bDirty = false;
}

