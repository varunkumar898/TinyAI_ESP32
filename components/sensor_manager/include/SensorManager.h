#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "PinManager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read-only sensors layered on top of the pin manager.
 *
 * Digital sensors are ordinary GPIO inputs owned by pin_manager. ADC sensors
 * are limited to the ESP32 ADC1 pins (GPIO32..39) and keep the pad in input
 * mode. A sensor never owns a pin and never writes a GPIO: removing or
 * resetting a sensor leaves the pin configuration untouched.
 */

#define SENSOR_MAX 12
#define SENSOR_NAME_MAX 24
#define SENSOR_CONFIG_SCHEMA_VERSION 1

typedef enum {
    SENSOR_KIND_DIGITAL = 0,
    SENSOR_KIND_ADC1 = 1,
} sensor_kind_t;

typedef enum {
    SENSOR_OK = 0,
    SENSOR_ERR_INVALID_ARGUMENT,
    SENSOR_ERR_NO_SPACE,
    SENSOR_ERR_NOT_FOUND,
    SENSOR_ERR_NAME_CONFLICT,
    SENSOR_ERR_NAME_INVALID,
    SENSOR_ERR_PIN_NOT_READY,
    SENSOR_ERR_NOT_INPUT,
    SENSOR_ERR_NOT_ADC1,
    SENSOR_ERR_ADC_USES_PULL,
    SENSOR_ERR_HARDWARE,
    SENSOR_ERR_BAD_CONFIG,
} sensor_status_t;

typedef struct {
    char name[SENSOR_NAME_MAX];
    sensor_kind_t kind;
    uint8_t pin;
    bool valid;            /* last sample succeeded */
    uint32_t last_sample_ms;
    int32_t raw;           /* ADC raw counts (0..4095), or 0/1 for digital */
    int32_t value;         /* digital 0/1, or approximate ADC millivolts */
} sensor_sample_t;

/* Creates the internal mutex and clears the registry. Safe to call twice. */
void sensor_manager_init(void);

/* Forgets every sensor and releases the ADC unit. Does not touch GPIOs. */
sensor_status_t sensor_manager_reset(void);

/* The GPIO must already be initialized as an input by pin_manager. ADC
 * sensors additionally require an ADC1-capable pin and no internal pull. */
sensor_status_t sensor_manager_add(const char *name, sensor_kind_t kind, uint8_t pin);
sensor_status_t sensor_manager_remove(const char *name);

/* Cached record. Call sensor_manager_sample() to refresh it. */
sensor_status_t sensor_manager_get(const char *name, sensor_sample_t *out);
size_t sensor_manager_list(sensor_sample_t *out, size_t capacity);

/* Reads the hardware now and updates the cached record. */
sensor_status_t sensor_manager_sample(const char *name, sensor_sample_t *out);
/* Samples every configured sensor. Returns the number sampled successfully. */
size_t sensor_manager_sample_all(void);

/* Convenience for automation: returns 0 and valid=false on error. */
int32_t sensor_manager_value(const char *name, bool *valid);

const char *sensor_kind_name(sensor_kind_t kind);
const char *sensor_status_name(sensor_status_t status);

/* Versioned payload. Schema 1:
 *   u16 version, u8 count,
 *   count * { u8 name_len, name[], u8 kind, u8 pin }
 * Import validates every record before replacing the current registry. */
size_t sensor_manager_export_config(uint8_t *buffer, size_t capacity,
                                    sensor_status_t *status);
sensor_status_t sensor_manager_import_config(const uint8_t *buffer, size_t length);

#ifdef SENSOR_MANAGER_HOST_TEST
/* Deterministic host ADC source, in raw 12-bit counts. */
void sensor_manager_test_set_adc_raw(uint8_t pin, uint16_t raw);
void sensor_manager_test_set_time(uint32_t now_ms);
#endif

#ifdef __cplusplus
}
#endif
