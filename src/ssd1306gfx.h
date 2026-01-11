// ssd1306gfx.h
// Graphics overlay for SSD1306 waveform display
// Works by directly manipulating the lower half of the framebuffer

#ifndef _ssd1306gfx_h
#define _ssd1306gfx_h

#include <circle/i2cmaster.h>
#include <circle/types.h>
#include <cstring>

// SSD1306 commands for partial update
enum TSSD1306GfxCommand : u8
{
	GfxSetColumnAddress = 0x21,
	GfxSetPageAddress   = 0x22,
	GfxSetStartLine     = 0x40,
};

class CSSD1306Gfx
{
public:
class CSSD1306Gfx
{
public:
	static constexpr unsigned WIDTH = 128;
	static constexpr unsigned WAVEFORM_HEIGHT = 32;
	static constexpr unsigned BUFFER_SIZE = WIDTH * WAVEFORM_HEIGHT / 8;  // 512 bytes

	CSSD1306Gfx (CI2CMaster *pI2CMaster, u8 nAddress, unsigned nParamsHeight)
		: m_pI2CMaster (pI2CMaster),
		  m_nAddress (nAddress),
		  m_nHeight (nParamsHeight)
	{
		Clear ();
	}

	// Clear the waveform framebuffer
	void Clear (void)
	{
		memset (m_FrameBuffer, 0, BUFFER_SIZE);
	}

	// Set a single pixel (x: 0-127, y: 0-31 relative to waveform area)
	void SetPixel (unsigned x, unsigned y, bool bOn = true)
	{
		if (x >= WIDTH || y >= WAVEFORM_HEIGHT)
			return;

		// Framebuffer is organized as: columns of 8-pixel vertical strips
		// Each byte represents 8 vertical pixels, bit 0 = top, bit 7 = bottom
		unsigned nByteOffset = (y / 8) * WIDTH + x;
		unsigned nBit = y % 8;

		if (bOn)
			m_FrameBuffer[nByteOffset] |= (1 << nBit);
		else
			m_FrameBuffer[nByteOffset] &= ~(1 << nBit);
	}

	// Send the waveform framebuffer content to the display (bottom 32 rows)
	void UpdateDisplay (void)
	{
		// Only update waveform area if screen is large enough
		if (m_nHeight < 64) return;

		// Set column address range: 0-127
		WriteCommand (GfxSetColumnAddress);
		WriteCommand (0x00);
		WriteCommand (WIDTH - 1);

		// Set page address range: 4-7 (pages for y=32-63)
		WriteCommand (GfxSetPageAddress);
		WriteCommand (4);  // Page 4 = y 32-39
		WriteCommand (7);  // Page 7 = y 56-63

		// Write framebuffer data with data prefix byte
		u8 packet[1 + BUFFER_SIZE];
		packet[0] = 0x40;  // Data control byte (Co=0, D/C=1)
		memcpy (packet + 1, m_FrameBuffer, BUFFER_SIZE);
		
		m_pI2CMaster->Write (m_nAddress, packet, sizeof(packet));
	}

	// Draw MIDI status bar on the absolute bottom page of the display
	void DrawMidiStatus (const unsigned *pActiveNotes)
	{
		u8 statusBar[WIDTH];
		memset(statusBar, 0, WIDTH);

		// 8 Tone Generators, 16 pixels each
		for (unsigned tg = 0; tg < 8; tg++)
		{
			if (pActiveNotes[tg] > 0)
			{
				// Draw 12-pixel bar with 2-pixel padding
				unsigned startX = (tg * 16) + 2;
				for (unsigned i = 0; i < 12; i++)
				{
					// Draw line at bottom (bit 7)
					statusBar[startX + i] = 0x80; 
				}
			}
		}

		// Calculate target page (Page 3 for 32px, Page 7 for 64px)
		u8 targetPage = (m_nHeight / 8) - 1;

		// Set column address range: 0-127
		WriteCommand (GfxSetColumnAddress);
		WriteCommand (0x00);
		WriteCommand (WIDTH - 1);

		// Set page address range: Target Page only
		WriteCommand (GfxSetPageAddress);
		WriteCommand (targetPage);
		WriteCommand (targetPage);

		// Write status line
		u8 packet[1 + WIDTH];
		packet[0] = 0x40;
		memcpy (packet + 1, statusBar, WIDTH);
		
		m_pI2CMaster->Write (m_nAddress, packet, sizeof(packet));
	}

private:
	void WriteCommand (u8 nCommand)
	{
		u8 buffer[2] = { 0x80, nCommand };  // Command control byte followed by command
		m_pI2CMaster->Write (m_nAddress, buffer, 2);
	}

private:
	CI2CMaster *m_pI2CMaster;
	u8 m_nAddress;
	unsigned m_nHeight;
	u8 m_FrameBuffer[BUFFER_SIZE];
};

#endif
