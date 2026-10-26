#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "typed_command.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_CAP_NONE        = 0,
    PIN_CAP_OUTPUT      = (1u << 0),
    PIN_CAP_INPUT       = (1u << 1),
    PIN_CAP_ADC1        = (1u << 2),
    PIN_CAP_DAC         = (1u << 3),
    PIN_CAP_PWM         = (1u << 4),
    PIN_CAP_PROTECTED   = (1u << 5), /* Strapping pins: GPIO 0, 2, 5, 12, 15 */
    PIN_CAP_FLASH       = (1u << 6), /* Flash SPI: GPIO 6-11 */
    PIN_CAP_CONSOLE     = (1u << 7), /* UART0 console: GPIO 1, 3 */
} pin_cap_mask_t;

typedef enum {
    VALIDATE_OK = 0,
    VALIDATE_ERR_NONEXISTENT_PIN,
    VALIDATE_ERR_FLASH_BLOCKED,
    VALIDATE_ERR_CONSOLE_BLOCKED,
    VALIDATE_ERR_PROTECTED_PIN,
    VALIDATE_ERR_INPUT_ONLY,
    VALIDATE_ERR_NOT_ADC1,
    VALIDATE_ERR_NOT_DAC,
    VALIDATE_ERR_INVALID_PWM,
    VALIDATE_ERR_INVALID_UART,
    VALIDATE_ERR_UNSUPPORTED_INTENT,
} validator_status_t;

typedef struct {
    validator_status_t status;
    char reason[80];
} validator_result_t;

/* Get capability bitmask for a pin (0..39) */
uint32_t validator_get_pin_caps(uint8_t pin);

/* Validate a typed command against the static capability table */
validator_result_t validator_validate(const typed_command_t *cmd, bool allow_protected);

const char *validator_status_name(validator_status_t status);

#ifdef __cplusplus
}
#endif
