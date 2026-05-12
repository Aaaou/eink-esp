#include "pcf8574_device.h"

Pcf8574Device::Pcf8574Device(i2c_master_bus_handle_t i2c_bus, uint8_t addr, uint8_t initial_latch)
    : I2cDevice(i2c_bus, addr), latch_(initial_latch) {
    i2c_master_transmit(i2c_device_, &latch_, 1, 100);
}

void Pcf8574Device::Write(uint8_t value) {
    latch_ = value;
    ESP_ERROR_CHECK(i2c_master_transmit(i2c_device_, &latch_, 1, 100));
}

void Pcf8574Device::SetBit(uint8_t bit, bool high) {
    uint8_t next = latch_;
    if (high) {
        next |= static_cast<uint8_t>(1U << bit);
    } else {
        next &= static_cast<uint8_t>(~(1U << bit));
    }
    Write(next);
}
