# ESP32 TinyAI Controller

Firmware for a classic ESP32-WROOM-32 (ESP-IDF, C17) that combines a small
offline-trained intent classifier with deterministic, safety-checked GPIO control.
There is no cloud inference and no Python at device runtime.

## What it is (and is not)

- TinyML, not a generative LLM. The model is a quantized linear softmax
  classifier over hashed word/character n-grams. It emits one of seven intent
  labels plus confidence and margin.
- The model never touches hardware. Every actuation passes through
  `pin_manager` and the bare-metal GPIO driver.
- AI is optional. Console GPIO control, sensors, automation, and storage work
  with `CONFIG_TINYAI_ENABLED=n`.

## Seven intents

`TURN_ON`, `TURN_OFF`, `TOGGLE`, `GET_STATUS`, `LIST_DEVICES`, `HELP`, `UNKNOWN`.
Configuration is deliberately not an AI intent.

## Safety policy

- Deterministic pre-filter runs before inference: empty/too-long/non-ASCII,
  negation, hypothetical/question, prompt injection/role-play, and configuration
  language are rejected.
- Actuation requires confidence >= 0.65 and margin >= 0.15 (stricter than the
  model's own 0.50 / 0.10 thresholds).
- Pin manager is the sole ownership/safety layer. Uninitialized, reserved,
  ambiguous, unauthorized, or low-confidence commands do not actuate.
- Multi-target ON/TOGGLE require an explicit local `confirm`. `outputs all on`
  affects only test-approved outputs; `outputs all off` affects configured
  outputs.
- Automation rules never write on pause/disable/reset.

## GPIO driver

`GPIO_Init`, `GPIO_Write`, `GPIO_Toggle`, `GPIO_Read`, `GPIO_SetPull`,
`GPIO_DeInit` are implemented with direct register, IO_MUX, and GPIO-matrix
access. `gpio_set_level()` / `gpio_set_direction()` are not used.

## Sensors and automation

- Digital sensors are ordinary GPIO inputs owned by `pin_manager`.
- ADC sensors use ADC1 only (GPIO32-39), input mode, no pull, approximate
  uncalibrated millivolts.
- Automation rules: one sensor, one configured output, above/below threshold
  with hysteresis. Disable/pause/reset leave output state unchanged.

## Configuration storage

`config_store` persists three CRC-enveloped NVS blobs (`pins`, `sensors`,
`rules`) under namespace `tinyai_cfg` with magic `TACF`. Pin load is required;
sensor/rule failures return `CFG_PARTIAL` while the pin set stays loaded.

## Model card

- Architecture: hashed n-grams -> 2048 buckets -> 7-class linear softmax (int8).
- Test accuracy: 0.8301886792452831
- Macro F1: 0.8319330808740085
- Challenge accuracy: 0.8285714285714286 (29/35)
- Int8 weights: 14336 bytes; model: 14364 bytes.
- Feature buckets: 2048; classes: 7.

## Build

Set up ESP-IDF v5.5, then:

```
idf.py build
```

AI-disabled build:

```
idf.py -B build_noai -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.noai" build
```

(where `sdkconfig.noai` contains `CONFIG_TINYAI_ENABLED=n`).

## Host tests

```
cmake -S tests/host -B build_host -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang
cmake --build build_host
./build_host/host_tests.exe
```

Current result: 3430 assertions, 0 failures.

Host inference benchmark (100000 iterations, rotating phrases):

- 12.119 us/prediction, 82515 predictions/s on the build host.
- These are host numbers, not ESP32 measurements.

## Console commands (summary)

Pins, devices, groups, AI, sensors, automation, rules, storage, system.
See `help` on the device for the full list. Natural language is entered with
`do <text>` or `ai <text>`; `classify <text>` runs inference without actuation.

## Hardware test status

No ESP32 board was available during development. GPIO, ADC, NVS-on-device, and
on-device inference latency are unverified. No hardware results are claimed.
