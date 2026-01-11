// waveformringbuffer.h
// Lock-free SPSC ring buffer for audio waveform display
// Audio thread writes samples, UI thread reads snapshots

#ifndef _waveformringbuffer_h
#define _waveformringbuffer_h

#include <stdint.h>

class CWaveformRingBuffer
{
public:
	static constexpr unsigned BUFFER_SIZE = 256;     // Power of 2 for fast modulo
	static constexpr unsigned WAVEFORM_WIDTH = 128;  // Display width in pixels

	CWaveformRingBuffer ()
		: m_nWriteIndex (0)
	{
		for (unsigned i = 0; i < BUFFER_SIZE; i++)
		{
			m_Buffer[i] = 0;
		}
	}

	// Called by audio thread - writes one sample
	void WriteSample (int8_t sample)
	{
		m_Buffer[m_nWriteIndex & (BUFFER_SIZE - 1)] = sample;
		m_nWriteIndex++;
	}

	// Called by UI thread - copies WAVEFORM_WIDTH samples to buffer
	// Returns the most recent samples for display
	void GetSnapshot (int8_t *pBuffer)
	{
		unsigned nEnd = m_nWriteIndex;
		unsigned nStart = nEnd - WAVEFORM_WIDTH;
		
		for (unsigned i = 0; i < WAVEFORM_WIDTH; i++)
		{
			pBuffer[i] = m_Buffer[(nStart + i) & (BUFFER_SIZE - 1)];
		}
	}

private:
	volatile int8_t m_Buffer[BUFFER_SIZE];
	volatile unsigned m_nWriteIndex;
};

#endif
