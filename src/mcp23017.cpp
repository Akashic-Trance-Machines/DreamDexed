// mcp23017.cpp
// MCP23017 I2C I/O Expander driver for DreamDexed

#include "mcp23017.h"

// MCP23017 register addresses (BANK=0 mode, default)
static constexpr uint8_t IODIRA   = 0x00;
static constexpr uint8_t IODIRB   = 0x01;
static constexpr uint8_t IPOLA    = 0x02;
static constexpr uint8_t IPOLB    = 0x03;
static constexpr uint8_t GPINTENA = 0x04;
static constexpr uint8_t GPINTENB = 0x05;
static constexpr uint8_t DEFVALA  = 0x06;
static constexpr uint8_t DEFVALB  = 0x07;
static constexpr uint8_t INTCONA  = 0x08;
static constexpr uint8_t INTCONB  = 0x09;
static constexpr uint8_t IOCON    = 0x0A;  // Also at 0x0B
static constexpr uint8_t GPPUA    = 0x0C;
static constexpr uint8_t GPPUB    = 0x0D;
static constexpr uint8_t INTFA    = 0x0E;
static constexpr uint8_t INTFB    = 0x0F;
static constexpr uint8_t INTCAPA  = 0x10;
static constexpr uint8_t INTCAPB  = 0x11;
static constexpr uint8_t GPIOA    = 0x12;
static constexpr uint8_t GPIOB    = 0x13;

CMCP23017::CMCP23017(CI2CMaster& i2c, uint8_t addr)
    : m_I2C(i2c),
      m_Addr(addr)
{
}

bool CMCP23017::WriteReg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    int result = m_I2C.Write(m_Addr, buf, sizeof(buf));
    return result == sizeof(buf);
}

bool CMCP23017::ReadReg(uint8_t reg, uint8_t& val)
{
    // Write register address
    int result = m_I2C.Write(m_Addr, &reg, 1);
    if (result != 1) {
        return false;
    }
    
    // Read register value
    result = m_I2C.Read(m_Addr, &val, 1);
    return result == 1;
}

bool CMCP23017::Init_UI_PortA(uint8_t inputMaskA)
{
    // IOCON configuration:
    // Bit 6: MIRROR=1 (INT pins are OR'd together, both fire for any interrupt)
    // Bit 2: ODR=1 (Open-Drain output for INT pins)
    // Bit 1: INTPOL=0 (Active-low interrupt)
    // Value: 0b01000100 = 0x44
    if (!WriteReg(IOCON, 0x44)) return false;

    // Set direction: 1 = input
    if (!WriteReg(IODIRA, inputMaskA)) return false;

    // Enable internal pull-ups on inputs
    if (!WriteReg(GPPUA, inputMaskA)) return false;

    // Interrupt-on-change: compare to previous value (INTCON=0)
    if (!WriteReg(INTCONA, 0x00)) return false;
    
    // Enable interrupt-on-change for input pins
    if (!WriteReg(GPINTENA, inputMaskA)) return false;

    // Clear any pending interrupt by reading both INTCAP and GPIO
    (void)ReadIntcapA();
    (void)ReadGpioA();
    return true;
}

bool CMCP23017::Init_UI_PortB(uint8_t inputMaskB)
{
    // IOCON already configured by Init_UI_PortA, but write again for safety
    // MIRROR=1, ODR=1, INTPOL=0
    if (!WriteReg(IOCON, 0x44)) return false;

    // Set direction: 1 = input
    if (!WriteReg(IODIRB, inputMaskB)) return false;

    // Enable internal pull-ups on inputs
    if (!WriteReg(GPPUB, inputMaskB)) return false;

    // Interrupt-on-change: compare to previous value (INTCON=0)
    if (!WriteReg(INTCONB, 0x00)) return false;
    
    // Enable interrupt-on-change for input pins
    if (!WriteReg(GPINTENB, inputMaskB)) return false;

    // Clear any pending interrupt by reading both INTCAP and GPIO
    (void)ReadIntcapB();
    (void)ReadGpioB();
    return true;
}

uint8_t CMCP23017::ReadIntcapA()
{
    uint8_t val = 0xFF;  // Default to all high (unpressed)
    (void)ReadReg(INTCAPA, val);
    return val;
}

uint8_t CMCP23017::ReadIntcapB()
{
    uint8_t val = 0xFF;
    (void)ReadReg(INTCAPB, val);
    return val;
}

uint8_t CMCP23017::ReadGpioA()
{
    uint8_t val = 0xFF;
    (void)ReadReg(GPIOA, val);
    return val;
}

uint8_t CMCP23017::ReadGpioB()
{
    uint8_t val = 0xFF;
    (void)ReadReg(GPIOB, val);
    return val;
}