#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Board profile: ESP32 DevKit V1, 30-pin style, ESP32-WROOM-32 module.
 *
 * The 30-pin layout is common but not standardized. Clone boards can route
 * different pins, add/remove GPIO16/17, fit a different flash size, or omit
 * pull-up resistors. This profile blocks pins that are unsafe on every
 * WROOM-32 board and documents the assumptions in docs/gpio_pinout.md.
 */

#define BOARD_PROFILE_PIN_COUNT 40
#define BOARD_PROFILE_NAME "ESP32 DevKit V1 30-pin (WROOM-32, common layout)"
#define BOARD_PROFILE_FLASH_ASSUMED_BYTES (4u * 1024u * 1024u)
#define BOARD_PROFILE_PSRAM_ASSUMED false

typedef enum {
    BOARD_PIN_OK = 0,
    BOARD_PIN_NOT_PRESENT,
    BOARD_PIN_FLASH,
    BOARD_PIN_STRAPPING,
    BOARD_PIN_UART0,
    BOARD_PIN_MODULE_RESERVED,
    BOARD_PIN_INPUT_ONLY,
    BOARD_PIN_NO_PULL,
    BOARD_PIN_INVALID,
} board_pin_issue_t;

typedef struct {
    bool present;
    bool input_capable;
    bool output_capable;
    bool pull_up;
    bool pull_down;
    bool adc1;
    bool adc2;
    bool boot_strap;
    bool reserved;
    bool module_reserved;
    const char *reserved_reason;
} board_pin_caps_t;

typedef struct {
    const char *board_name;
    const char *module_name;
    uint32_t flash_bytes;
    bool psram_present;
    uint32_t free_heap_bytes;
    uint32_t minimum_free_heap_bytes;
    uint32_t largest_free_block_bytes;
    uint8_t chip_cores;
    uint16_t chip_revision;
} board_runtime_info_t;

/* Logs chip, flash, PSRAM and heap facts at boot (ESP-IDF target only). */
void board_profile_init(void);

/* Safe on host: fills runtime facts with values supplied by the platform. */
void board_profile_get_runtime_info(board_runtime_info_t *info);

/* Capability query; never allocates. */
bool board_profile_get_pin_caps(uint8_t pin, board_pin_caps_t *caps);

/*
 * Validate a board-level use of a pin. This is the policy gate used by both
 * the register driver and the pin manager.
 */
board_pin_issue_t board_profile_check_pin(uint8_t pin, bool output, bool pull_up,
                                          bool pull_down);

const char *board_profile_issue_name(board_pin_issue_t issue);
const char *board_profile_issue_hint(board_pin_issue_t issue);

/* Human-readable pin table line for the serial console. */
void board_profile_format_pin(uint8_t pin, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
