# Resume Project: ESP32 TinyAI Controller

## Project Title

ESP32 TinyAI Controller: Offline Intent Classification with Safe GPIO Control

## Technology Stack

- ESP32-WROOM-32
- ESP-IDF v5.5
- C17
- FreeRTOS
- Bare-metal GPIO register access
- TinyML hashed n-gram linear classifier
- NVS configuration storage
- UART command console
- Host-side C test suite

## Summary

Firmware for a classic ESP32-WROOM-32 that runs a small offline-trained intent
classifier and applies its results through a deterministic, safety-checked GPIO
control layer. The model is a quantized linear softmax classifier, not a
generative language model. Natural-language input is limited to seven intent
labels and never bypasses pin-manager authorization.

## Embedded C and GPIO Driver Contributions

- Implemented `GPIO_Init`, `GPIO_Write`, `GPIO_Toggle`, `GPIO_Read`,
  `GPIO_SetPull`, and `GPIO_DeInit` using direct register, IO_MUX, and
  GPIO-matrix access.
- Added a pin manager for initialization, ownership, grouping, locking, and
  authorization checks.
- Added safety checks for reserved, input-only, unauthorized, and
  low-confidence GPIO actions.

## TinyML Inference Implementation

- Hashed word and character n-grams into 2048 feature buckets.
- Quantized linear softmax classifier with seven intent classes.
- Int8 weights: 14,336 bytes; total model: 14,364 bytes.
- Reported evaluation: test accuracy 0.8301886792452831, macro F1
  0.8319330808740085, challenge accuracy 29/35.

## Safety Architecture

- Deterministic pre-inference filtering for negation, hypotheticals,
  prompt-injection patterns, and configuration language.
- Stricter actuation thresholds: confidence >= 0.65 and margin >= 0.15.
- Multi-target ON/TOGGLE requires local confirmation.
- Automation rules leave output state unchanged on pause, disable, or reset.

## Firmware Build and Validation

- ESP-IDF firmware build succeeds.
- AI-disabled firmware build succeeds.
- Host test suite: 3,430 assertions, 0 failures.
- Host benchmark: 12.119 microseconds per prediction, 82,515 predictions per
  second on the build host.

## Known Limitations

- Physical ESP32 hardware was not available for validation.
- GPIO, ADC, NVS-on-device, and on-device inference latency remain unverified.
- True source-level exclusion of TinyML code when `CONFIG_TINYAI_ENABLED=n` is
  not fully achieved; model components are still compiled but runtime AI is
  disabled.
