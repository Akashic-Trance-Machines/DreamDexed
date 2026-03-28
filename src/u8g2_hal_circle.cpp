//
// u8g2_hal_circle.cpp
//
// DreamDexed - u8g2 HAL callbacks for Circle OS (bare-metal RPi)
//
// u8g2 is used in buffer-only mode: all rendering goes to an internal
// RAM buffer. The buffer is then memcpy'd to the CSSD1306Device framebuffer.
// Therefore, the I2C byte callback is a no-op. Only delays are needed
// for u8g2's internal init sequence.
//
#include <circle/timer.h>

extern "C" {
#include "u8g2/u8g2.h"
}

// Byte communication callback — no-op (we don't use u8g2's I2C)
extern "C" uint8_t u8x8_byte_circle_noop(u8x8_t *u8x8, uint8_t msg,
                                          uint8_t arg_int, void *arg_ptr)
{
	(void)u8x8;
	(void)msg;
	(void)arg_int;
	(void)arg_ptr;
	return 1; // success
}

// GPIO and delay callback — only delays are implemented
extern "C" uint8_t u8x8_gpio_and_delay_circle(u8x8_t *u8x8, uint8_t msg,
                                               uint8_t arg_int, void *arg_ptr)
{
	(void)u8x8;
	(void)arg_ptr;

	switch (msg)
	{
	case U8X8_MSG_DELAY_MILLI:
		CTimer::SimpleMsDelay(arg_int);
		break;

	case U8X8_MSG_DELAY_10MICRO:
		CTimer::SimpleusDelay(arg_int * 10);
		break;

	case U8X8_MSG_DELAY_100NANO:
		// Best effort — Circle timer resolution may not reach 100ns
		CTimer::SimpleusDelay(1);
		break;

	default:
		break;
	}

	return 1;
}
