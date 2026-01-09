// mcp23017.h
// MCP23017 I2C I/O Expander driver for DreamDexed
#pragma once

#include <circle/i2cmaster.h>
#include <stdint.h>

class CMCP23017 {
public:
    CMCP23017(CI2CMaster& i2c, uint8_t addr = 0x21);

    // Initialize Port A/B for UI inputs
    // inputMask: 1 bits = inputs (e.g. 0b00011111 for bits 0-4)
    bool Init_UI_PortA(uint8_t inputMaskA);
    bool Init_UI_PortB(uint8_t inputMaskB);
    
    // Read interrupt capture registers (clears interrupt, returns latched state)
    uint8_t ReadIntcapA();
    uint8_t ReadIntcapB();
    
    // Read GPIO registers directly
    uint8_t ReadGpioA();
    uint8_t ReadGpioB();

private:
    bool WriteReg(uint8_t reg, uint8_t val);
    bool ReadReg(uint8_t reg, uint8_t& val);

    CI2CMaster& m_I2C;
    uint8_t m_Addr;
};