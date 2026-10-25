#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register-level GPIO driver for the original ESP32 (ESP32-WROOM-32).
 *
 * This driver deliberately does not call ESP-IDF gpio_set_level(),
 * gpio_set_direction(), gpio_config() or gpio_pad_select_gpio(). It writes the
 * ESP32 GPIO/IO_MUX registers described in GPIO_Registers.h. All user-facing
 * GPIO changes must go through pin_manager, which enforces board policy,
 * ownership, polarity and AI authorization.
 */

typedef enum {
    GPIO_OK = 0,
    GPIO_ERR_INVALID_PIN,
    GPIO_ERR_RESERVED,
    GPIO_ERR_UNSUPPORTED_MODE,
    GPIO_ERR_UNSUPPORTED_PULL,
    GPIO_ERR_ALREADY_INITIALIZED,
    GPIO_ERR_NOT_INITIALIZED,
    GPIO_ERR_NOT_OUTPUT,
    GPIO_ERR_NOT_INPUT,
    GPIO_ERR_INVALID_ARGUMENT,
    GPIO_ERR_CONFLICT,
    GPIO_ERR_HARDWARE
} GPIO_Status;

/* Compatibility names used by the safety layer. They intentionally remain
 * distinct enum values so callers can report a useful failure reason. */
#define GPIO_STATUS_OK GPIO_OK
#define GPIO_STATUS_INVALID_PIN GPIO_ERR_INVALID_PIN
#define GPIO_STATUS_BOARD_BLOCKED GPIO_ERR_RESERVED
#define GPIO_STATUS_UNSUPPORTED_MODE GPIO_ERR_UNSUPPORTED_MODE
#define GPIO_STATUS_UNSUPPORTED_PULL GPIO_ERR_UNSUPPORTED_PULL
#define GPIO_STATUS_ALREADY_INITIALIZED GPIO_ERR_ALREADY_INITIALIZED
#define GPIO_STATUS_NOT_INITIALIZED GPIO_ERR_NOT_INITIALIZED
#define GPIO_STATUS_NOT_OUTPUT GPIO_ERR_NOT_OUTPUT
#define GPIO_STATUS_NOT_INPUT GPIO_ERR_NOT_INPUT
#define GPIO_STATUS_INVALID_ARGUMENT GPIO_ERR_INVALID_ARGUMENT
#define GPIO_STATUS_CONFLICT GPIO_ERR_CONFLICT
#define GPIO_STATUS_HARDWARE GPIO_ERR_HARDWARE

typedef enum {
    GPIO_MODE_INPUT = 0,
    GPIO_MODE_OUTPUT,
    GPIO_MODE_INPUT_PULLUP,
    GPIO_MODE_INPUT_PULLDOWN
} GPIO_Mode;

typedef enum {
    GPIO_LEVEL_LOW = 0,
    GPIO_LEVEL_HIGH = 1
} GPIO_Level;

typedef enum {
    GPIO_PULL_NONE = 0,
    GPIO_PULL_UP,
    GPIO_PULL_DOWN
} GPIO_Pull;

typedef struct {
    bool exists;              /* physically exposed on the DevKit V1 30-pin */
    bool input_capable;
    bool output_capable;
    bool pullup_capable;      /* GPIO34-39 have no software pulls */
    bool pulldown_capable;
    bool adc1_capable;
    bool adc2_capable;
    bool rtc_capable;
    bool reserved;            /* flash, bootstrap, UART0, unverified module pin */
} GPIO_Capabilities;

/* Required public API. */
GPIO_Status GPIO_Init(uint8_t pin, GPIO_Mode mode);
GPIO_Status GPIO_Write(uint8_t pin, GPIO_Level level);
GPIO_Status GPIO_Toggle(uint8_t pin);
GPIO_Status GPIO_Read(uint8_t pin, uint8_t *value);
GPIO_Status GPIO_SetPull(uint8_t pin, GPIO_Pull pull);
GPIO_Status GPIO_DeInit(uint8_t pin);

/* Extended API used by the safety layer and group controller.
 *
 * GPIO_InitFull validates board policy, initializes the pad and sets the
 * output latch before enabling the pad driver, avoiding a LOW glitch on
 * active-low relay boards during boot. GPIO_InitWithLevel is the same
 * operation without an explicit pull. GPIO_WriteMasks performs one
 * W1TS/W1TC pair for low pins and one pair for GPIO32/33. Bits 34-39 are
 * rejected because those pads are input-only.
 */
GPIO_Status GPIO_InitFull(uint8_t pin, GPIO_Mode mode, GPIO_Pull pull,
                          GPIO_Level initial_level);
GPIO_Status GPIO_InitWithLevel(uint8_t pin, GPIO_Mode mode, GPIO_Level initial_level);
GPIO_Status GPIO_WriteMask(uint64_t set_mask, uint64_t clear_mask);
GPIO_Status GPIO_WriteMasks(uint64_t set_mask, uint64_t clear_mask);
GPIO_Status GPIO_GetCapabilities(uint8_t pin, GPIO_Capabilities *caps);
GPIO_Status GPIO_GetMode(uint8_t pin, GPIO_Mode *mode);
GPIO_Status GPIO_GetPull(uint8_t pin, GPIO_Pull *pull);
GPIO_Status GPIO_ValidatePin(uint8_t pin, GPIO_Mode mode, GPIO_Pull pull,
                             const char **reason);
bool GPIO_IsReserved(uint8_t pin);
const char *GPIO_StatusName(GPIO_Status status);
const char *GPIO_ModeName(GPIO_Mode mode);
const char *GPIO_PullName(GPIO_Pull pull);

#ifdef GPIO_DRIVER_HOST_TEST
/* Test-only hook used by GPIO_SimReset/GPIO_TestResetAll. */
void GPIO_TestResetDriverState(void);
#endif

#ifdef __cplusplus
}
#endif
