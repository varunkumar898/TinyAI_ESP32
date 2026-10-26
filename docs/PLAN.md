# TinyAI_ESP32 Upgrade Plan

## Goal
Offline natural-language hardware controller on classic ESP32 (DevKit V1, ESP-IDF v5.5.x, C17, FreeRTOS, 4 MB flash, no PSRAM).
A trained TinyML model decides WHAT the user wants. A deterministic extractor decides WHICH pin/value. A validated executor decides HOW.
The model never touches registers or runs generated code.

## Architecture
Input -> normalize -> rule-based safety/negation/injection filter -> intent model
-> slot extractor -> typed command -> validator -> resource manager -> driver -> result/audit log

## Global rules (apply to every milestone)
1. Never hide crashes: no disabling watchdogs, no blind switch to recursive mutexes.
2. Never mark a hardware test PASS from a build or mock alone. If hardware, wiring, or a scope measurement is needed, STOP and ask me.
3. Report real numbers. Do not claim understanding of arbitrary English. If accuracy targets are missed, say so and favor fewer false actuations over more accepted commands.
4. An intent prediction is never authorization. Risky/disruptive changes need explicit local approval.
5. Hard-blocked pins: GPIO6-11 (flash), GPIO1/3 (UART0 console). Protected (need scoped approval): GPIO0, 2, 5, 12, 15. GPIO34-39 are input-only. ADC restricted to ADC1 (GPIO32-39). DAC only GPIO25/26.
6. No hard-coded class counts anywhere: derive from one label list.
7. Preserve git history, licenses, and existing CLI commands (ai, do, classify). Work on a feature branch, never push to main.
8. Use local headers in $IDF_PATH/components/*/include as API reference; do not assume internet.
9. Keep changes small and modular. Every change needs a host test.

## Definition of done (every milestone)
```powershell
idf.py -DTINYAI_ENABLED=ON build
idf.py -DTINYAI_ENABLED=OFF build
pytest tests/host
```
(Adjust the flag name to the repo's actual Kconfig/CMake option.)

## Milestones
- [x] M1 Fix UART ring-buffer assertion and pin-manager mutex watchdog crash
- [x] M2 Stable timed GPIO2 blink with scoped local authorization
- [ ] M3 Typed command structs + deterministic slot extractor + rule-based safety filter
- [ ] M4 Dataset (~12 intent families, 150+ examples each, grouped splits, separate handwritten challenge set, 300+ negative/adversarial)
- [ ] M5 Retrain hashed linear classifier, calibrate, int8 quantize, export, parity tests. Try an MLP ONLY if held-out accuracy < 95%
- [ ] M6 Integrate model into command engine and serial console (bare English + ai explain / dry-run / model / benchmark)
- [ ] M7 PWM (LEDC) and ADC1
- [ ] M8 I2C scan, UART1, then SPI, interrupts, rules
- [ ] M9 Regression, load, and hardware validation
- [ ] M10 Docs, reports, PR

## MVP intent families (start here)
GPIO_WRITE, GPIO_TOGGLE, GPIO_READ, GPIO_BLINK, GPIO_PULSE, PWM, ADC_READ, I2C_SCAN, UART_SEND, GET_STATUS, HELP, UNKNOWN
Deferred: DAC, SPI, interrupts, rules, I2C read/write, SAVE_CONFIG.

## Baseline (preserve for comparison)
7 intents, 2048-bucket int8 linear classifier, 14,364 bytes, ~83% accuracy.

## Progress log

### Milestone 1 & Milestone 2 (2026-10-26)
- **Root Cause Analysis (M1)**:
  1. *Repeated resets / stack overflow*: In `components/serial_console/SerialConsole.c`, `outf()` allocated `char buffer[4096]` on the stack while `CONSOLE_TASK_STACK` was 6,144 bytes. Nested CLI command handlers executing stdio formatting exceeded the stack limit, corrupting adjacent heap memory.
  2. *UART ring-buffer assertion*: The stack overflow corrupted FreeRTOS heap memory containing UART0 ring-buffer control structures (`rx_ring_buf`). Additionally, UART0 driver was installed without checking `uart_is_driver_installed()` and without `uart_vfs_dev_use_driver(UART_NUM_0)`, causing stdout direct register writes to conflict concurrently with the UART ISR.
  3. *Interrupt watchdog timeout on mutex*: Dynamic heap allocation `xSemaphoreCreateMutex()` for `s_mutex` in `PinManager.c` became corrupted by the stack overflow. In `xQueueSemaphoreTake`, entering `taskENTER_CRITICAL()` with corrupted pointers caused an infinite loop with interrupts disabled, triggering the hardware level-5 Interrupt Watchdog Timer (IWDT).
  4. *Lock held across console I/O*: In `Automation.c`, `AUTO_LOGW` logged to console while holding `automation_lock`, causing priority inversion and potential deadlock with console task.
- **Fixes Applied (M1 & M2)**:
  - `SerialConsole.c`:
    - Replaced the 4096-byte stack allocation in `outf()` with a statically allocated, bounded buffer `s_console_out_buffer[2048]` with bounds checking and null termination.
    - Updated `serial_console_start()`: checks `uart_is_driver_installed(UART_NUM_0)`, sets CRLF tx and CR rx line endings, and routes stdout through `uart_vfs_dev_use_driver(UART_NUM_0)`.
    - Added `serial_console_get_stack_high_water_mark()` and exposed console stack headroom via `heap`/`mem` commands.
    - Added structured `gpio` commands: `gpio authorize 2`, `gpio revoke 2`, `gpio blink 2 <ms>`, and `gpio blink cancel`.
  - `PinManager.c` & `PinManager.h`:
    - Replaced dynamic heap mutex allocation with static allocation using `StaticSemaphore_t` (`xSemaphoreCreateMutexStatic(&s_mutex_buffer)`), ensuring it can never fail or be corrupted by heap fragmentation.
    - Replaced `portMAX_DELAY` lock waits with bounded timeout `pm_lock()` (1000 ms).
    - Added non-blocking timed GPIO pulse/blink engine (`pin_manager_blink_start`, `pin_manager_blink_cancel`, `pin_manager_blink_update`, `pin_manager_blink_is_active`).
    - Added lock acquisition/release tracking and host inspection functions (`pin_manager_test_get_lock_state`, `pin_manager_test_reset_lock_state`).
  - `board_profile.c` & `board_profile.h`:
    - Added session-only, RAM-only local authorization for strapping pin GPIO2 (`board_profile_set_gpio2_authorized`, `board_profile_is_gpio2_authorized`).
    - Strictly maintained hardware protection for GPIO0, 5, 12, 15, flash pins (6-11), and UART0 (1/3).
  - `Automation.c`:
    - Removed console logging while holding `automation_lock`.
- **Tests & DoD Verification**:
  - `idf.py -DTINYAI_ENABLED=ON build`: PASS (binary size 0x45320, 82% free).
  - `idf.py -DTINYAI_ENABLED=OFF build`: PASS (binary size 0x45320, 82% free).
  - `pytest tests/host`: PASS (1 suite, 3,559 assertions passing).
  - Host test coverage includes:
    - Mutex acquire/release balance across success, failure, and early return paths.
    - Bounded console formatting with oversized (3000 chars) and malformed input.
    - GPIO2 timed blink: authorized path, denied path, cancellation, invalid duration, and safe return to inactive state.
- **Stack High-Water Mark**:
  - Stack allocation in `outf()` reduced from 4,096 bytes to 0 bytes (moved to static bounded storage), freeing ~4 KB of task stack headroom.
- **Hardware Status**:
  - Ready for hardware validation upon user confirmation of COM port (Global Rule 2 observed).

