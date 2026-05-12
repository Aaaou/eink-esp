# EINK-C3 Board Notes

## Wake word

EINK-C3 currently keeps local wake word detection disabled by default.

Although ESP32-C3 can select `CONFIG_USE_ESP_WAKE_WORD`, enabling the
WakeNet9s model directly on this board can fail at runtime after Wi-Fi,
MQTT/TLS, audio, assets, and display tasks are already running. The observed
failure is:

```text
Item psram alloc failed
EspWakeWord::Initialize
AudioService::EnableWakeWordDetection
```

This is caused by insufficient runtime memory for the ESP-SR WakeNet model in
the current EINK-C3 configuration. The ESP-HI board can enable wake word on C3
because its board profile applies aggressive memory reductions, including lower
Wi-Fi buffer counts, disabled Wi-Fi enterprise support, smaller LWIP task
settings, no console output, size optimization, and a lighter ADC/PDM audio
path.

Do not enable wake word on EINK-C3 by only adding:

```text
CONFIG_USE_ESP_WAKE_WORD=y
```

If wake word support is needed later, treat it as a separate memory-reduction
experiment. Start by comparing against `main/boards/esp-hi/config.json`, and
verify runtime free SRAM after MQTT connection before enabling WakeNet.
