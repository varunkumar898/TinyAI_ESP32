#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "intents.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMD_UART_PAYLOAD_MAX 64
#define CMD_HELP_TOPIC_MAX 32
#define CMD_UNKNOWN_REASON_MAX 64

/*
 * Typed command payloads.
 * Each command carries only fields relevant to its intent.
 */

typedef struct {
    uint8_t pin;
    uint8_t level; /* 0 = LOW/off, 1 = HIGH/on */
} cmd_gpio_write_t;

typedef struct {
    uint8_t pin;
} cmd_gpio_toggle_t;

typedef struct {
    uint8_t pin;
} cmd_gpio_read_t;

typedef struct {
    uint8_t pin;
    uint32_t on_ms;
    uint32_t off_ms;
    uint32_t repeats; /* 0 = run until cancelled / default 1 */
} cmd_gpio_blink_t;

typedef struct {
    uint8_t pin;
    uint32_t duration_ms;
} cmd_gpio_pulse_t;

typedef struct {
    uint8_t pin;
    uint32_t freq_hz;
    uint8_t duty_pct; /* 0 - 100 */
} cmd_pwm_t;

typedef struct {
    uint8_t pin;
    bool raw_counts; /* false = millivolts, true = raw counts */
} cmd_adc_read_t;

typedef struct {
    uint8_t bus; /* 0 = I2C0, 1 = I2C1 */
} cmd_i2c_scan_t;

typedef struct {
    uint8_t uart_id; /* 0, 1, 2 */
    uint32_t baud;   /* e.g. 115200 */
    char payload[CMD_UART_PAYLOAD_MAX];
    size_t length;
} cmd_uart_send_t;

typedef struct {
    int16_t pin; /* -1 = all / system status, >= 0 = specific pin */
} cmd_get_status_t;

typedef struct {
    char topic[CMD_HELP_TOPIC_MAX];
} cmd_help_t;

typedef struct {
    char reason[CMD_UNKNOWN_REASON_MAX];
} cmd_unknown_t;

/* Tagged union keyed by intent */
typedef struct {
    intent_t intent;
    union {
        cmd_gpio_write_t gpio_write;
        cmd_gpio_toggle_t gpio_toggle;
        cmd_gpio_read_t gpio_read;
        cmd_gpio_blink_t gpio_blink;
        cmd_gpio_pulse_t gpio_pulse;
        cmd_pwm_t pwm;
        cmd_adc_read_t adc_read;
        cmd_i2c_scan_t i2c_scan;
        cmd_uart_send_t uart_send;
        cmd_get_status_t get_status;
        cmd_help_t help;
        cmd_unknown_t unknown;
    } params;
} typed_command_t;

/* Zero-initializing constructors per variant */
typed_command_t cmd_make_gpio_write(uint8_t pin, uint8_t level);
typed_command_t cmd_make_gpio_toggle(uint8_t pin);
typed_command_t cmd_make_gpio_read(uint8_t pin);
typed_command_t cmd_make_gpio_blink(uint8_t pin, uint32_t on_ms, uint32_t off_ms, uint32_t repeats);
typed_command_t cmd_make_gpio_pulse(uint8_t pin, uint32_t duration_ms);
typed_command_t cmd_make_pwm(uint8_t pin, uint32_t freq_hz, uint8_t duty_pct);
typed_command_t cmd_make_adc_read(uint8_t pin, bool raw_counts);
typed_command_t cmd_make_i2c_scan(uint8_t bus);
typed_command_t cmd_make_uart_send(uint8_t uart_id, uint32_t baud, const char *payload, size_t length);
typed_command_t cmd_make_get_status(int16_t pin);
typed_command_t cmd_make_help(const char *topic);
typed_command_t cmd_make_unknown(const char *reason);

#ifdef __cplusplus
}
#endif
