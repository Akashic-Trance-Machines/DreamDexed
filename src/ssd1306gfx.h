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
	GfxSetMemoryAddressingMode = 0x20,
	GfxSetStartLine     = 0x40,
};

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

	// Draw a vertical line for thicker waveform (x: 0-127, y1/y2: 0-31)
	void DrawVLine (unsigned x, unsigned y1, unsigned y2)
	{
		if (x >= WIDTH) return;
		if (y1 > y2) { unsigned t = y1; y1 = y2; y2 = t; }
		for (unsigned y = y1; y <= y2 && y < WAVEFORM_HEIGHT; y++)
		{
			SetPixel(x, y);
		}
	}

	// Draw MIDI status bars into the framebuffer (bottom row of waveform area)
	void DrawMidiStatusIntoBuffer (const unsigned *pActiveNotes)
	{
		// Draw into the bottom 2 pixels of the waveform area (y=30-31)
		for (unsigned tg = 0; tg < 8; tg++)
		{
			if (pActiveNotes[tg] > 0)
			{
				// Draw 12-pixel bar with 2-pixel padding
				unsigned startX = (tg * 16) + 2;
				for (unsigned i = 0; i < 12; i++)
				{
					SetPixel(startX + i, 30);
					SetPixel(startX + i, 31);
				}
			}
		}
	}

	// Send the waveform framebuffer content to the display (bottom 32 rows)
	void UpdateDisplay (void)
	{
		// Only update waveform area if screen is large enough
		if (m_nHeight < 64) return;

		// Switch to Horizontal Addressing Mode for bulk write
		WriteCommand (GfxSetMemoryAddressingMode);
		WriteCommand (0x00);

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

		// Reset column and page ranges to full screen for standard driver
		// (Stay in Horizontal Addressing Mode as Circle's SSD1306 driver uses it too)
		WriteCommand (GfxSetColumnAddress);
		WriteCommand (0x00);
		WriteCommand (WIDTH - 1);
		WriteCommand (GfxSetPageAddress);
		WriteCommand (0);
		WriteCommand (7);
		
		// Reset start line to 0 to ensure text isn't shifted
		WriteCommand (0x40);
	}

	// Legacy method for standalone MIDI status (deprecated, use DrawMidiStatusIntoBuffer)
	void DrawMidiStatus (const unsigned *pActiveNotes)
	{
		// Now just calls the buffer version and updates display
		// This maintains compatibility but integrates into waveform area
		DrawMidiStatusIntoBuffer(pActiveNotes);
		UpdateDisplay();
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

