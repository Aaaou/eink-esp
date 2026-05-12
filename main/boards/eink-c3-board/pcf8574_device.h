#ifndef _PCF8574_DEVICE_H_
#define _PCF8574_DEVICE_H_

#include "i2c_device.h"

class Pcf8574Device : public I2cDevice {
public:
    Pcf8574Device(i2c_master_bus_handle_t i2c_bus, uint8_t addr, uint8_t initial_latch = 0xFF);

    void Write(uint8_t value);
    void SetBit(uint8_t bit, bool high);
    uint8_t GetLatch() const { return latch_; }

private:
    uint8_t latch_;
};

#endif // _PCF8574_DEVICE_H_
