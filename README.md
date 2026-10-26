# ESP32 TinyAI Controller

Firmware for the ESP32-WROOM-32 (ESP-IDF v5.5, C17) integrating an offline-trained TinyML intent classifier with deterministic, register-level bare-metal GPIO control, sensor management, and persistent configuration storage.

The system processes natural language commands locally without cloud connectivity or runtime Python execution. The machine learning model acts strictly as an intent classifier; all hardware actuation passes through a multi-tier deterministic safety layer and the pin ownership manager.

---

## Project Overview

Modern microcontroller applications frequently require natural user interfaces, but running generative language models locally on resource-constrained microcontrollers like the ESP32 is impractical. The ESP32 TinyAI Controller addresses this challenge by deploying a lightweight, int8-quantized linear classifier optimized for edge execution.

### Key Characteristics

- **Offline TinyML, Not a Generative LLM**: The inference engine is an int8-quantized linear softmax classifier operating over hashed character and word n-grams. It classifies input text into one of seven discrete intent labels. It does not generate text.
- **Hardware Isolation**: The machine learning model possesses zero direct hardware access. Actuation is decoupled from inference and gated by safety policies, confidence/margin validation, and pin ownership checks.
- **Direct Register-Level GPIO**: High-speed GPIO driver implemented via direct ESP32 register manipulation (`GPIO_OUT_W1TS_REG`, `GPIO_OUT_W1TC_REG`, `GPIO_IN_REG`, IO_MUX, and GPIO Matrix registers), bypassing `gpio_set_level()` and `gpio_set_direction()`.
- **Dual-Build Architecture**: The firmware supports compilation with TinyML enabled or disabled (`CONFIG_TINYAI_ENABLED=n`). When disabled, deterministic UART console control, sensors, automation rules, and NVS configuration storage operate with reduced footprint.
- **Deterministic Pre-Filtering**: Input text is evaluated before inference to reject negations, hypotheticals, prompt-injection patterns, and configuration commands.

---

## Architecture

The firmware is structured into modular ESP-IDF components with clean interfaces:

```
+-----------------------------------------------------------------------+
|                         UART Serial Console                           |
+-----------------------------------+-----------------------------------+
                                    |
                    [ Natural Language Command ]
                                    v
+-----------------------------------------------------------------------+
|                       Command Engine Pipeline                         |
|                                                                       |
|  1. Pre-Inference Safety Filter                                       |
|     - Rejects: Non-ASCII, oversized (>160 bytes), negation ("don't"),|
|       hypotheticals ("what if"), prompt injections, config syntax     |
|                                                                       |
|  2. TinyML Inference Engine                                           |
|     - Feature Hashing: 3-5 char n-grams + word tokens -> 2048 buckets |
|     - Quantized Linear Softmax (int8 weights, int32 biases in flash)  |
|     - Emits: Intent Class + Confidence + Margin                       |
|                                                                       |
|  3. Actuation Policy Gate                                             |
|     - Minimum Thresholds: Confidence >= 0.65, Margin >= 0.15          |
|     - Multi-Target Confirmation: Requires explicit local `confirm`    |
+-----------------------------------+-----------------------------------+
                                    |
                       [ Authorized Actuation ]
                                    v
+-----------------------------------------------------------------------+
|                             Pin Manager                               |
|  - Pin Ownership & Capability Enforcement (Strapping / Flash protect) |
|  - Direction & Active-Low Safe Latching                               |
|  - Origin Permissions (Local Console vs. AI vs. Automation Rule)      |
|  - Independent Pin Locking & Output Group Management                  |
+-----------------------------------+-----------------------------------+
                                    |
                        [ Low-Level Operations ]
                                    v
+-----------------------------------------------------------------------+
|                      Bare-Metal GPIO Driver                           |
|  - Direct W1TS / W1TC / IN Register Access                            |
|  - IO_MUX and GPIO Matrix Configuration                               |
+-----------------------------------+-----------------------------------+
                                    |
                        [ Hardware Pins 0-39 ]
                                    v
                           ESP32-WROOM-32 MCU
```

### Auxiliary Subsystems

- **Sensor Manager**: Manages digital GPIO sensors (buttons, PIR) and ADC1 analog sensors (GPIO32-39) with raw counts and millivolt conversion.
- **Automation Engine**: FreeRTOS background task (250 ms cycle) executing rules with configurable thresholds, hysteresis, and hold times. Invariant: Rules never drive outputs on pause, disable, or reset.
- **Config Store**: Persists pins, sensors, and rules in Non-Volatile Storage (NVS) using CRC32-validated binary envelopes under namespace `tinyai_cfg`.

---

## Implemented Features

### 1. Bare-Metal GPIO Subsystem
- Functions implemented: `GPIO_Init`, `GPIO_Write`, `GPIO_Toggle`, `GPIO_Read`, `GPIO_SetPull`, and `GPIO_DeInit`.
- Direct register manipulation via `GPIO_OUT_W1TS_REG`, `GPIO_OUT_W1TC_REG`, `GPIO_IN_REG`, `GPIO_ENABLE_W1TS_REG`, `GPIO_ENABLE_W1TC_REG`, and IO_MUX pad registers.
- Output mask writes split cleanly across GPIO 0-31 and GPIO 32-33 banks.
- Active-low safety latch: Pins configured as active-low output latch HIGH to `W1TS` before enabling the pad output, preventing power-on relay glitches.
- Hardware protection: Rejects initialization of strapping pins (GPIO 0, 2, 12, 15) in conflicting modes, flash SPI bus pins (GPIO 6-11), and input-only pins (GPIO 34-39) for output.

### 2. Pin Manager and Access Control
- Independent pin lifecycle management, ownership tracking, and name/alias resolution.
- Granular permission flags: `allow_local` (UART console) and `allow_ai` (TinyML commands).
- Pin locking mechanism preventing manual or automated modification until explicitly unlocked.
- Output group management allowing atomic multi-pin actions.

### 3. Offline TinyML Inference
- 2048 feature buckets computed via murmur-style hashed word tokens and 3-5 character n-grams.
- 7 output intent classes:
  - `TURN_ON`
  - `TURN_OFF`
  - `TOGGLE`
  - `GET_STATUS`
  - `LIST_DEVICES`
  - `HELP`
  - `UNKNOWN`
- Int8 quantized model: 14,336 bytes of weight parameters, 28 bytes of int32 bias, 14,364 bytes total model size.
- Zero runtime dynamic heap allocation (`malloc`/`free`); model resides entirely in flash.

### 4. Safety Architecture and Gating
- Pre-filter stage drops malformed input, non-ASCII characters, negation phrasing, hypothetical queries, jailbreak attempts, and configuration directives.
- Stricter actuation threshold: While the classifier threshold is 0.50 confidence and 0.10 margin, hardware actuation requires confidence >= 0.65 and margin >= 0.15.
- Multi-target safety barrier: Commands affecting multiple pins (or groups) require an explicit local `confirm` command within a timeout window before actuation.

### 5. Sensor and Automation Subsystem
- Digital sensor inputs with debouncing and button finite-state machine (FSM).
- ADC1 support (GPIO 32-39) with raw sample reading and approximate uncalibrated millivolt calculation.
- Dynamic automation rules supporting `ABOVE` and `BELOW` conditions with hysteresis bands.
- Timed hold support for PIR motion sensors.

### 6. Configuration Storage
- NVS storage under namespace `tinyai_cfg` with magic identifier `TACF`.
- Individual CRC32 checksums for pin, sensor, and rule configuration blocks.
- Partial configuration recovery: If sensor or rule data is corrupted, pin configurations are preserved while failed subsystems enter a safe default state (`CFG_PARTIAL`). All outputs are driven OFF on boot.

---

## Hardware Requirements

| Component | Specification | Notes |
|:---|:---|:---|
| **Microcontroller** | ESP32-WROOM-32 | Xtensa Dual-Core 32-bit LX6 @ 240 MHz, 520 KB SRAM, 4 MB SPI Flash |
| **Interface** | USB-to-UART Bridge | CP2102, CH340, or FT232 running at 115200 baud (8-N-1) |
| **Default Test Pin** | GPIO23 (`led1`) | Harmless test output configured at first boot |
| **Digital I/O** | GPIO 4, 16, 17, 18, 19, 21, 22, 25, 26, 27, 32, 33 | User-configurable for inputs, outputs, buttons, or relays |
| **Analog Inputs** | GPIO 32, 33, 34, 35, 36, 39 | ADC1 channels only (ADC2 is reserved/avoided due to Wi-Fi multiplexing) |

---

## Software Requirements

- **ESP-IDF**: Version v5.5 (tested on v5.5.6)
- **Host Build System**: CMake 3.16+ and Ninja
- **Target Toolchain**: Xtensa ESP32 GCC toolchain (managed by ESP-IDF)
- **Host Test Toolchain**: GCC (MinGW-w64 on Windows, GCC on Linux) or Clang with C17 support
- **Python**: Version 3.10 - 3.12 (with `numpy` for training and evaluation tools)

---

## Installation

### 1. ESP-IDF Setup

Follow Espressif's documentation to install ESP-IDF v5.5:

```bash
# Linux/macOS
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32
. ./export.sh
```

```powershell
# Windows (PowerShell)
cd C:\esp
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
.\install.ps1 esp32
. .\export.ps1
```

### 2. Clone the Repository

```bash
git clone https://github.com/varunkumar898/TinyAI_ESP32.git
cd TinyAI_ESP32
```

### 3. Python Environment for Tools

```bash
python -m venv .venv

# Linux/macOS
source .venv/bin/activate

# Windows
.\.venv\Scripts\Activate.ps1

pip install numpy
```

---

## Build and Flash

### Standard Firmware Build (TinyAI Enabled)

```bash
# Set target chip to ESP32
idf.py set-target esp32

# Compile firmware
idf.py build

# Flash to board and open serial monitor (replace PORT with your serial port, e.g., COM3 or /dev/ttyUSB0)
idf.py -p PORT flash monitor
```

### AI-Disabled Firmware Build

To build the firmware without TinyML natural language processing (reducing flash footprint while retaining console GPIO, sensor, automation, and storage capabilities):

```bash
idf.py -B build_noai -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.noai" build
idf.py -B build_noai -p PORT flash monitor
```

*Note: `sdkconfig.noai` contains `CONFIG_TINYAI_ENABLED=n`.*

### Exit the Serial Monitor

Press `Ctrl + ]` to exit `idf.py monitor`.

---

## GPIO Control

The GPIO subsystem directly interacts with the ESP32 hardware registers:

```c
/* Pin initialization */
GPIO_Init(23, GPIO_MODE_OUTPUT, GPIO_PULL_NONE);

/* High-speed atomic output write */
GPIO_Write(23, GPIO_LEVEL_HIGH); // Sets bit in GPIO_OUT_W1TS_REG
GPIO_Write(23, GPIO_LEVEL_LOW);  // Sets bit in GPIO_OUT_W1TC_REG

/* Atomic toggle */
GPIO_Toggle(23);

/* Direct pad state read */
gpio_level_t level = GPIO_Read(23); // Reads bit from GPIO_IN_REG

/* Safe release */
GPIO_DeInit(23);
```

### Safety Features
- **No Floating Outputs**: Active-low configurations drive `W1TS` before setting output direction, preventing low-side relays from pulsing during initialization.
- **Strapping Protection**: Rejects modification of pins tied to boot mode selection unless explicitly bypassed.
- **Flash Bus Protection**: Pins 6 through 11 (SPI0/1 flash interface) are permanently locked.

---

## TinyML Model Architecture

The classifier is designed for extreme memory efficiency and deterministic execution:

```
[ Input Text (<= 160 bytes) ]
              |
              v
[ Text Normalization & Tokenization ]
  - Lowercase, punctuation stripping
  - Word unigrams + Character n-grams (range: 3 to 5 characters)
              |
              v
[ Feature Hashing (Murmur-inspired 32-bit hash) ]
  - Dimension: 2048 feature buckets
  - Clamped term counts
              |
              v
[ Quantized Linear Softmax Layer ]
  - Weights: 2048 buckets x 7 classes = 14,336 int8 weights
  - Biases:  7 classes x int32 = 28 bytes
  - Logits:  Accumulated in int32, scaled back to float probabilities
              |
              v
[ Output: Intent Probabilities, Top Class, Confidence, Margin ]
```

### Model Size Profile

- **Weight Storage**: 14,336 bytes (int8)
- **Bias Storage**: 28 bytes (int32)
- **Model Total Size**: 14,364 bytes
- **RAM Allocation**: 0 bytes dynamic heap during inference (stack buffer used for accumulation)

---

## Model Training and Evaluation

The model is trained using Python scripts in the `tools/` directory.

### Training the Model

```bash
python tools/train.py --epochs 700 --lr 0.03
```

### Evaluating the Model

```bash
python tools/evaluate.py
```

### Reproducible Evaluation Metrics

Measurements obtained from `model/metrics.json` and `model/evaluation.json`:

#### Test Split (159 samples, group-stratified):
- **Test Accuracy**: 0.830188 (83.02%)
- **Macro F1 Score**: 0.831933 (83.19%)
- **Macro Precision**: 0.832468
- **Macro Recall**: 0.836254

#### Per-Class Performance:
| Intent Class | Precision | Recall | F1 Score | Support |
|:---|:---:|:---:|:---:|:---:|
| `TURN_ON` | 0.912 | 0.838 | 0.873 | 37 |
| `TURN_OFF` | 0.714 | 0.690 | 0.702 | 29 |
| `TOGGLE` | 0.857 | 1.000 | 0.923 | 18 |
| `GET_STATUS` | 0.967 | 0.935 | 0.951 | 31 |
| `LIST_DEVICES` | 0.800 | 0.667 | 0.727 | 6 |
| `HELP` | 0.900 | 1.000 | 0.947 | 9 |
| `UNKNOWN` | 0.677 | 0.724 | 0.700 | 29 |

#### Challenge Set Performance (`data/challenge.csv`, 35 adversarial / challenging samples):
- **Challenge Accuracy**: 29 / 35 (82.86%)

---

## Console Commands

Connect via serial terminal at 115200 baud to interact with the command line interface:

### Pin and Device Management
| Command | Arguments | Description |
|:---|:---|:---|
| `pin init` | `<gpio> <in\|out> [none\|up\|down]` | Initialize a hardware pin |
| `pin deinit` | `<gpio>` | Deinitialize and safely float pin |
| `pin write` | `<gpio> <0\|1>` | Write logic level to pin |
| `pin toggle` | `<gpio>` | Toggle pin level |
| `pin read` | `<gpio>` | Read current logic level |
| `pin lock` | `<gpio>` | Lock pin against modification |
| `pin unlock` | `<gpio>` | Unlock pin |
| `device assign` | `<gpio> <name> [active_high] [allow_local] [allow_ai]` | Assign logical name to pin |
| `device alias` | `<gpio> <alias>` | Add alias to device |
| `device list` | - | List all configured devices |

### Natural Language and TinyML
| Command | Arguments | Description |
|:---|:---|:---|
| `ai <text>` | `<natural language>` | Execute natural language command via TinyML |
| `do <text>` | `<natural language>` | Synonym for `ai` |
| `classify <text>` | `<natural language>` | Run classifier without actuating hardware |
| `confirm` | - | Confirm a pending multi-target actuation |
| `cancel` | - | Cancel pending actuation |

### Sensors and Automation
| Command | Arguments | Description |
|:---|:---|:---|
| `sensor create` | `<name> <gpio> <digital\|adc1> [pull]` | Register a new sensor |
| `sensor read` | `<name>` | Read sensor sample |
| `sensor list` | - | List active sensors |
| `rule create` | `<name> <sensor> <output> <above\|below> <thresh> <hyst>` | Create automation rule |
| `rule pause` | `<name>` | Pause automation rule |
| `rule resume` | `<name>` | Resume automation rule |
| `rule list` | - | List automation rules |

### Storage and System
| Command | Arguments | Description |
|:---|:---|:---|
| `save` | - | Persist current configuration to NVS |
| `load` | - | Reload configuration from NVS |
| `status` | - | Display system telemetry and heap statistics |
| `benchmark` | - | Run on-device inference speed benchmark |
| `help` | - | Display command assistance |

---

## Sensors and Automation

```
                     +----------------------+
                     | Sensor Sample (ADC1) |
                     +----------+-----------+
                                |
                                v
                   [ Value > High Threshold ? ]
                   /                          \
             YES  /                            \  NO
                 v                              v
      +---------------------+        [ Value < Low Threshold ? ]
      |  Trigger Actuation  |        /                         \
      +---------------------+  YES  /                           \  NO
                                   v                             v
                         +-------------------+         +-------------------+
                         | Deactivate Output |         | Retain Prev State |
                         |   (Hysteresis)    |         |    (Deadband)     |
                         +-------------------+         +-------------------+
```

- **Hysteresis Bands**: Prevents relay chattering on analog sensors.
- **PIR Hold Time**: Configurable hold time ensures motion detection outputs stay asserted for a specified duration before resetting.
- **State Guarantee**: Pausing, disabling, or resetting automation rules leaves the controlled output in its last stable physical state without driving arbitrary changes.

---

## Safety and GPIO Restrictions

The system implements a four-stage defense-in-depth safety pipeline:

1. **Deterministic Textual Gating**:
   - Negation filter: Sentences containing "don't", "never", "not", "stop" are rejected.
   - Hypothetical filter: "what if", "would it" queries are rejected.
   - Prompt injection guard: Rejects "ignore previous instructions", "system:", "admin mode".
   - Configuration guard: Rejects natural language attempts to reconfigure pins.
2. **Inference Margin Validation**:
   - Actuation requires `confidence >= 0.65` and `margin >= 0.15` (gap between top-1 and top-2 probabilities).
3. **Pin Manager Authorization**:
   - Commands targeting pins without `allow_ai = true` are rejected.
   - Locked pins are completely write-protected.
4. **Multi-Target Confirmation**:
   - Commands targeting groups or multiple devices require an operator to issue the `confirm` command within 15 seconds.

---

## Test Results

The project includes an extensive host test suite running natively on the development host with simulated GPIO registers:

```bash
# Build host test runner
cmake -S tests/host -B build_host -G "MinGW Makefiles" -DCMAKE_C_COMPILER=gcc
cmake --build build_host

# Run test runner
./build_host/host_tests
```

### Test Suite Execution
- **Host Test Assertions**: 3,430 assertions
- **Test Failures**: 0 failures
- **Coverage**: Entity extraction, tokenizer buckets, int8 logits vs. Python golden vectors, GPIO register simulation, active-low safety latching, pin manager authorization, rule hysteresis, NVS serialization/deserialization.

### Host Inference Benchmark
Measured using `./build_host/host_benchmark` (20,000 iterations over 10 rotating phrases on development workstation):
- **Latency**: 12.119 us to 20.017 us per prediction (host-dependent)
- **Throughput**: 49,957 to 82,515 predictions per second

> *Note: These figures represent host workstation performance (x86_64 / ARM64), not on-device ESP32 hardware measurements.*

---

## Memory and Performance

Measurements obtained from the ESP-IDF build output:

| Metric | Measured Value | Partition Limit | Utilization |
|:---|:---:|:---:|:---:|
| **App Binary Size** | 277,520 bytes (`0x43c10`) | 1,536,000 bytes (`0x177000`) | 18% used (82% free) |
| **Bootloader Size** | 26,000 bytes (`0x6590`) | 28,672 bytes (`0x7000`) | 91% used (9% free) |
| **Model Weight Data** | 14,364 bytes | - | Resides in Flash `.rodata` |
| **Dynamic Heap Usage** | ~35 KB allocated | 520 KB SRAM | > 280 KB free heap |

---

## Hardware Verification Status

The current status of physical hardware testing is explicitly reported below:

- [x] **ESP-IDF Firmware Build**: Compiles cleanly with ESP-IDF v5.5 (0 warnings, 0 errors).
- [x] **AI-Disabled Firmware Build**: Compiles cleanly (`CONFIG_TINYAI_ENABLED=n`).
- [x] **Host Test Suite**: 3,430 assertions passing with 0 failures.
- [x] **Model Training & Evaluation**: Reproducible metrics verified against Python pipeline.
- [ ] **Physical ESP32 Silicon Validation**: Not yet executed. Physical GPIO electrical verification, hardware ADC linearity, on-device NVS retention, and on-chip inference latency have not been measured on physical hardware.

---

## Repository Structure

```
TinyAI_ESP32/
├── .github/
│   └── workflows/
│       └── ci.yml                     # GitHub Actions CI (host tests & evaluation)
├── components/
│   ├── automation/                    # Background rule engine and button FSM
│   ├── baremetal_gpio/                # Direct register GPIO driver & register defs
│   ├── board_profile/                 # Board pin capabilities & strapping table
│   ├── command_engine/                # Natural language pipeline & actuation policy
│   ├── config_store/                  # NVS binary serializer with CRC32 checks
│   ├── device_registry/               # Logical device mappings & alias store
│   ├── entity_extractor/              # Text normalizer, pre-filter & entity parser
│   ├── model_data/                    # Embedded int8 weights & model header
│   ├── pin_manager/                   # Pin ownership, locking & authorization layer
│   ├── sensor_manager/                # Digital sensor & ADC1 sampling driver
│   ├── serial_console/                # Interactive UART REPL command processor
│   └── tinyml/                        # Hashed n-gram tokenizer & linear softmax
├── data/
│   ├── challenge.csv                  # 35 challenge/adversarial evaluation queries
│   └── dataset.csv                    # 795 training/evaluation labeled examples
├── docs/
│   └── RESUME_PROJECT.md              # Resume-ready project engineering overview
├── main/
│   ├── CMakeLists.txt                 # ESP-IDF main component configuration
│   ├── Kconfig.projbuild              # Project-level Kconfig options
│   └── main.c                         # Application entry point & FreeRTOS tasks
├── model/
│   ├── evaluation.json                # Quantized & challenge evaluation results
│   ├── intent_model.npz               # Trained float model checkpoint
│   ├── metrics.json                   # Comprehensive training and validation metrics
│   ├── model_manifest.json            # Model deployment manifest & checksums
│   └── split.json                     # Group-stratified train/val/test splits
├── tests/
│   ├── generated/                     # Golden test vectors generated by export tool
│   └── host/                          # Native C host test runner & benchmarks
├── tools/
│   ├── build_dataset.py               # Dataset generation and validation utility
│   ├── evaluate.py                    # Quantized model & challenge evaluation
│   ├── export_model.py                # C header and golden vector exporter
│   ├── model_pipeline.py              # Featurization, training & evaluation logic
│   ├── tinyai_features.py             # N-gram hashing and feature extraction
│   └── train.py                       # Host training script using Adam optimizer
├── .gitignore                         # Build and temporary file exclusions
├── CMakeLists.txt                     # Top-level ESP-IDF project CMake file
├── README.md                          # Main project documentation
├── sdkconfig.defaults                 # Default configuration for ESP-IDF
└── sdkconfig.noai                     # Configuration overrides to disable TinyML
```

---

## Limitations

1. **Classifier Scope**: The TinyML model is a linear classifier over 2048 hashed n-gram buckets. It does not possess reasoning capabilities or conversational memory. Out-of-vocabulary phrasing may fall back to `UNKNOWN`.
2. **Uncalibrated ADC**: ADC1 analog readings are currently uncalibrated; readings represent approximate millivolt ranges. For precision voltage sensing, ESP32 `esp_adc_cal` curve fitting should be added.
3. **No Dynamic Rule Scripting**: Automation rules are limited to single-sensor, single-actuator threshold comparisons with hysteresis.

---

## Future Development

1. **Physical Hardware Validation**: Connect ESP32-WROOM-32 to logic analyzer and oscilloscope to verify GPIO switching times, pad glitch-freedom during boot, and power consumption.
2. **On-Device Inference Profiling**: Benchmark cycle-exact execution time of the vectorizer and quantized matrix multiplication using the Xtensa `CCOUNT` register.
3. **Int4 Weight Quantization**: Explore 4-bit weight packing to reduce flash memory footprint from 14 KB down to ~7 KB.
4. **ESP32-S3 Vector Acceleration**: Port feature hashing and dot-product routines to ESP32-S3 using PIE (Processor Instruction Extension) vector instructions.
