# Project: ESP32 TinyAI Controller

## Project Title
**ESP32 TinyAI Controller: Offline TinyML Intent Classification with Register-Level GPIO and Defense-in-Depth Safety Architecture**

---

## Technology Stack
- **Target Platform**: ESP32-WROOM-32 (Xtensa Dual-Core 32-bit LX6 @ 240 MHz, 520 KB SRAM, 4 MB Flash)
- **Firmware Environment**: ESP-IDF v5.5, FreeRTOS, C17 Standard
- **Peripherals & Drivers**: Bare-metal register-level GPIO (`W1TS`/`W1TC`), IO_MUX, GPIO Matrix, ADC1, UART
- **Machine Learning**: Offline int8-quantized linear softmax classifier over 2048 hashed n-gram buckets
- **Storage**: Non-Volatile Storage (NVS) with CRC32 binary envelopes and partial recovery fallback
- **Tooling & Validation**: CMake, Ninja, Python 3.12 (NumPy), GCC / Clang host test runner

---

## Technical Project Summary
Engineered production-grade embedded C17 firmware for the ESP32 microcontroller combining an edge-optimized TinyML intent classifier with deterministic, register-level GPIO control, sensor management, and persistent configuration storage. The system enables natural language appliance and pin control entirely on-device without cloud connectivity, network infrastructure, or runtime Python overhead. Designed a four-tier defense-in-depth safety system ensuring that natural language inputs never bypass hardware ownership, authorization policies, or fail-safe latching invariants.

---

## Embedded C and GPIO Driver Contributions
- **Register-Level GPIO Driver**: Implemented `GPIO_Init`, `GPIO_Write`, `GPIO_Toggle`, `GPIO_Read`, `GPIO_SetPull`, and `GPIO_DeInit` via direct manipulation of ESP32 `GPIO_OUT_W1TS_REG`, `GPIO_OUT_W1TC_REG`, `GPIO_IN_REG`, `GPIO_ENABLE_W1TS_REG`, IO_MUX pad registers, and GPIO matrix registers, avoiding standard HAL function call overhead.
- **Active-Low Safe Latching Sequence**: Architected an initialization sequence for active-low outputs that drives high logic level to the set register (`W1TS`) *before* configuring the pad as an output, eliminating boot-time glitch pulses on relays and external actuators.
- **Silicon Protection Invariants**: Hardened the pin subsystem to reject configuration on SPI flash bus pins (GPIO 6-11), strapping pins (GPIO 0, 2, 12, 15) in conflicting modes, and input-only pads (GPIO 34-39) for output.
- **Pin Manager & Access Control**: Designed an ownership and capability management layer enforcing separate permissions for local console (`allow_local`) and machine learning (`allow_ai`), pin locking against unauthorized modification, and atomic output group operations.

---

## TinyML Inference Implementation
- **Lightweight Featurization Pipeline**: Implemented an on-device text tokenizer and Murmur-inspired feature hasher that transforms arbitrary natural language queries (up to 160 bytes) into a 2048-dimensional feature vector using character 3-to-5-grams and word tokens.
- **Quantized Linear Softmax Inference**: Developed an int8-quantized linear classifier outputting probabilities across 7 discrete intents (`TURN_ON`, `TURN_OFF`, `TOGGLE`, `GET_STATUS`, `LIST_DEVICES`, `HELP`, `UNKNOWN`).
- **Zero-Allocation Execution Path**: Stored 14,336 int8 weights and 28 int32 bias parameters in flash memory (`.rodata`, 14,364 bytes total model footprint). Inference runs with zero dynamic heap allocation (`malloc`/`free`), using a deterministic stack buffer for logit accumulation.

---

## Safety Architecture
- **Stage 1: Pre-Inference Deterministic Filtering**: Text normalizer intercepts and rejects non-ASCII characters, strings exceeding 160 bytes, negation constructs ("don't", "never", "not"), hypothetical/conditional questions ("what if I turn on"), prompt injections ("ignore previous instructions", "admin mode"), and configuration directives.
- **Stage 2: Strict Actuation Policy Gate**: Enforced an actuation threshold requiring model confidence >= 0.65 and class probability margin >= 0.15 (stricter than the classifier's internal 0.50/0.10 acceptance threshold).
- **Stage 3: Multi-Target Confirmation Barrier**: Natural language or group commands affecting multiple outputs generate an unconfirmed execution plan requiring an explicit operator `confirm` command within 15 seconds before driving pins.
- **Stage 4: Failsafe Non-Destructive Invariants**: Automation rules and sensor monitors never toggle or write to output pads upon rule pause, disable, or system reset.

---

## Firmware Build and Validation
- **Dual-Configuration Firmware**: Configured ESP-IDF build system with Kconfig toggles allowing the firmware to compile with full AI capabilities (`CONFIG_TINYAI_ENABLED=y`) or in AI-disabled mode (`CONFIG_TINYAI_ENABLED=n`) for ultra-low flash footprint.
- **NVS Configuration Store**: Built binary serializer persisting pin tables, sensor definitions, and automation rules under namespace `tinyai_cfg` with `TACF` magic header and individual CRC32 verification. Supports `CFG_PARTIAL` recovery mode to keep pins initialized even if rule storage is invalid.
- **Background Automation Task**: Integrated FreeRTOS background task running periodic 250 ms rule evaluation with configurable threshold hysteresis and button debounce state machines.

---

## Reproducible Host-Side Metrics
- **Host Test Suite**: 3,430 unit and integration assertions passing with zero failures across 72 test groups covering entity extraction, tokenizer parity, register simulation, and NVS serialization.
- **Host Inference Throughput**: 12.119 to 20.017 microseconds per prediction (49,957 to 82,515 predictions per second) on host development workstations.
- **Model Evaluation Accuracy** (Group-stratified test split, 159 rows):
  - Test Accuracy: 83.02% (`0.830188`)
  - Macro F1 Score: 83.19% (`0.831933`)
  - Per-Class F1: `GET_STATUS` (0.951), `HELP` (0.947), `TOGGLE` (0.923), `TURN_ON` (0.873), `LIST_DEVICES` (0.727), `TURN_OFF` (0.702), `UNKNOWN` (0.700).
- **Challenge Set Accuracy** (35 complex/adversarial queries): 29 / 35 (82.86%).
- **Firmware Flash Footprint**: 277 KB application binary (18% of 1.5 MB app partition, 82% free space).

---

## Known Limitations
- **Hardware Verification Status**: Electrical verification on physical ESP32 silicon has not yet been executed. Direct register GPIO writes, ADC1 reading, and on-chip latency are verified via host simulation and compilation, but remain physically unverified on hardware.
- **Linear Classifier Decision Boundary**: Being a linear classifier over hashed n-grams, out-of-vocabulary phrasing outside the training distribution falls back to `UNKNOWN`.
- **ADC Calibration**: Analog readings use uncalibrated raw ADC1 counts converted to approximate millivolts rather than factory eFuse two-point calibration.
