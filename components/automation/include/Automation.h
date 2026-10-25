#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "SensorManager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deterministic local sensor automation.
 *
 * PIR and button inputs are ordinary GPIO inputs owned by pin_manager. The
 * automation layer only observes them and performs output changes through
 * pin_manager with PM_ORIGIN_AUTOMATION. It never touches a GPIO register
 * directly, never uses the TinyML model, and never runs on a network input.
 *
 * PIR: motion turns the target ON and keeps it on until the input has been
 *      inactive for the configured hold time.
 * Button: one debounced press toggles the target.
 * Rules: one configured sensor and one configured output with an upper or
 *        lower threshold and hysteresis. Disabling, pausing or resetting a
 *        rule never writes its output; the operator keeps manual control.
 */

#define AUTOMATION_DEFAULT_PIR_HOLD_MS 5000u
#define AUTOMATION_MAX_RULES 8
#define AUTOMATION_RULE_NAME_MAX 24
#define AUTOMATION_CONFIG_SCHEMA_VERSION 1

typedef enum {
    AUTOMATION_ABOVE = 0,
    AUTOMATION_BELOW = 1,
} automation_condition_t;

typedef enum {
    AUTOMATION_RULE_OK = 0,
    AUTOMATION_RULE_INVALID_ARGUMENT,
    AUTOMATION_RULE_NAME_CONFLICT,
    AUTOMATION_RULE_NO_SPACE,
    AUTOMATION_RULE_NOT_FOUND,
    AUTOMATION_RULE_SENSOR_MISSING,
    AUTOMATION_RULE_OUTPUT_UNUSABLE,
} automation_rule_status_t;

typedef enum {
    AUTOMATION_CONFIG_OK = 0,
    AUTOMATION_CONFIG_NO_SPACE,
    AUTOMATION_CONFIG_BAD_CONFIG,
} automation_config_status_t;

typedef struct {
    char name[AUTOMATION_RULE_NAME_MAX];
    char sensor[SENSOR_NAME_MAX];
    uint8_t output_pin;
    automation_condition_t condition;
    int32_t threshold;
    uint32_t hysteresis;
    bool enabled;
    bool active;
    bool output_owned;
    bool fault;
    int32_t last_value;
    uint32_t last_sample_ms;
} automation_rule_info_t;

typedef struct {
    bool enabled;

    bool pir_bound;
    uint8_t pir_pin;
    uint8_t pir_output_pin;
    bool pir_active_high;
    uint32_t pir_hold_ms;
    bool pir_active;
    bool pir_output_owned;

    bool button_bound;
    uint8_t button_pin;
    uint8_t button_output_pin;
    bool button_active_low;
} automation_status_t;

void automation_init(void);

/* Removes all PIR/button bindings and threshold rules. Outputs are left in
 * their current state; the operator can switch them off explicitly. */
void automation_clear(void);

/* Pausing automation stops evaluations but never writes an output. */
void automation_set_enabled(bool enabled);
bool automation_is_enabled(void);

/* Returns true when the binding is valid and was accepted. A binding never
 * initializes, reconfigures or names a pin; callers must configure both pins
 * through pin_manager first. */
bool automation_bind_pir(uint8_t input_pin, bool active_high, uint8_t output_pin,
                         uint32_t hold_ms);
bool automation_bind_button(uint8_t input_pin, bool active_high,
                            uint8_t output_pin);

/* Threshold rules. The sensor must already exist in sensor_manager and the
 * output must be a configured, unlocked pin-manager output. Thresholds and
 * hysteresis use the sensor's reported units (ADC millivolts, or 0/1 for a
 * digital input). */
automation_rule_status_t automation_rule_add(const char *name, const char *sensor,
                                             uint8_t output_pin,
                                             automation_condition_t condition,
                                             int32_t threshold,
                                             uint32_t hysteresis);
automation_rule_status_t automation_rule_remove(const char *name);
automation_rule_status_t automation_rule_set_enabled(const char *name,
                                                     bool enabled);
/* Clears latched active/fault state without touching the output. */
automation_rule_status_t automation_rule_reset(const char *name);

size_t automation_rule_list(automation_rule_info_t *out, size_t capacity);
size_t automation_rule_count(void);

/* Versioned rule payload. Schema 1:
 *   u16 version, u8 count,
 *   count * { u8 name_len, name[], u8 sensor_len, sensor[],
 *             u8 output_pin, u8 condition, i32 threshold, u32 hysteresis,
 *             u8 enabled }
 * Import validates every rule before replacing the current set. Runtime
 * active/fault/ownership state is never restored. */
size_t automation_export_config(uint8_t *buffer, size_t capacity,
                                automation_config_status_t *status);
automation_config_status_t automation_import_config(const uint8_t *buffer,
                                                    size_t length);

const char *automation_rule_status_name(automation_rule_status_t status);

void automation_get_status(automation_status_t *out);

/* Samples both inputs and applies deterministic transitions. Call from one
 * polling task. Returns the number of successful output changes. */
size_t automation_update(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
