#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "GPIO_Driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pin manager: the single ownership layer for configurable GPIOs.
 *
 * The TinyML command engine, serial console, automation engine and any future
 * network interface all use this API. Direct GPIO driver access is reserved for
 * this component. Uninitialized pins cannot be driven, AI/remote commands must
 * target an explicitly AI-permitted configured device, and bulk ON operations
 * require confirmation.
 */

#define PM_MAX_PINS 40
#define PM_MAX_GROUPS 8
#define PM_MAX_ALIASES 4
#define PM_NAME_MAX 24
#define PM_GROUP_NAME_MAX 24
#define PM_LOG_CAPACITY 48
#define PM_LOG_TARGET_MAX 24
#define PM_LOG_INTENT_MAX 24
#define PM_LOG_RESULT_MAX 56
#define PM_LOG_SOURCE_MAX 16
#define PM_CONFIG_SCHEMA_VERSION 2

#define PM_CAP_INPUT 0x01u
#define PM_CAP_OUTPUT 0x02u
#define PM_CAP_PULL_UP 0x04u
#define PM_CAP_PULL_DOWN 0x08u
#define PM_CAP_ADC1 0x10u
#define PM_CAP_ADC2 0x20u

typedef enum {
    PM_OK = 0,
    PM_ERR_INVALID_ARGUMENT,
    PM_ERR_INVALID_PIN,
    PM_ERR_UNINITIALIZED,
    PM_ERR_ALREADY_INITIALIZED,
    PM_ERR_NOT_CONFIGURED,
    PM_ERR_ALREADY_CONFIGURED,
    PM_ERR_RESERVED,
    PM_ERR_LOCKED,
    PM_ERR_UNSUPPORTED,
    PM_ERR_DIRECTION,
    PM_ERR_NAME_CONFLICT,
    PM_ERR_NAME_INVALID,
    PM_ERR_NOT_FOUND,
    PM_ERR_NO_SPACE,
    PM_ERR_AI_DENIED,
    PM_ERR_CONFIRMATION,
    PM_ERR_HARDWARE,
    PM_ERR_BAD_CONFIG,
    PM_ERR_NOT_IMPLEMENTED,
} pm_status_t;

typedef enum {
    PM_ORIGIN_LOCAL = 0,   /* trusted serial console */
    PM_ORIGIN_AI = 1,      /* TinyML command engine */
    PM_ORIGIN_AUTOMATION = 2,
    PM_ORIGIN_REMOTE = 3,  /* treated as untrusted, same policy as AI */
} pm_origin_t;

typedef struct {
    uint8_t pin;
    bool present;
    bool initialized;
    bool configured;
    bool reserved;
    bool locked;
    bool ai_allowed;
    bool test_approved;
    bool active_high;
    bool output_capable;
    bool input_only;
    uint32_t capabilities;
    GPIO_Mode mode;
    GPIO_Pull pull;
    bool logical_state;
    bool physical_high;
    char device_name[PM_NAME_MAX];
    char aliases[PM_MAX_ALIASES][PM_NAME_MAX];
    uint8_t alias_count;
    char reserved_reason[64];
} pm_pin_status_t;

typedef struct {
    char name[PM_GROUP_NAME_MAX];
    uint64_t pin_mask;
    uint16_t pin_count;
} pm_group_status_t;

typedef struct {
    uint32_t seq;
    uint32_t timestamp_ms;
    pm_origin_t origin;
    int16_t pin;
    float confidence;
    char intent[PM_LOG_INTENT_MAX];
    char target[PM_LOG_TARGET_MAX];
    char source[PM_LOG_SOURCE_MAX];
    char result[PM_LOG_RESULT_MAX];
} pm_action_log_entry_t;

pm_status_t pin_manager_init(void);

/* Safe reset: logical OFF all initialized outputs, deinit every pin, clear
 * device/group configuration. Does not erase the audit log. */
pm_status_t pin_manager_reset(void);

pm_status_t pin_manager_pin_init(uint8_t pin, GPIO_Mode mode, GPIO_Pull pull);
pm_status_t pin_manager_pin_deinit(uint8_t pin);
pm_status_t pin_manager_assign(uint8_t pin, const char *name, bool active_high,
                               bool ai_allowed, bool test_approved);
pm_status_t pin_manager_release(uint8_t pin);
pm_status_t pin_manager_add_alias(uint8_t pin, const char *alias);
pm_status_t pin_manager_set_ai_allowed(uint8_t pin, bool allowed);
pm_status_t pin_manager_set_test_approved(uint8_t pin, bool approved);
pm_status_t pin_manager_set_locked(uint8_t pin, bool locked);

pm_status_t pin_manager_output(uint8_t pin, bool logical_on, pm_origin_t origin,
                               const char *intent, float confidence,
                               const char *source);
pm_status_t pin_manager_toggle(uint8_t pin, pm_origin_t origin, const char *intent,
                               float confidence, const char *source);
pm_status_t pin_manager_read(uint8_t pin, bool *physical_high, bool *logical_on);

/* Bulk operations use register masks. AI/remote ON with more than one target
 * requires confirmed=true. All-OFF only touches configured outputs. */
pm_status_t pin_manager_output_many(const uint8_t *pins, size_t count, bool logical_on,
                                    bool confirmed, pm_origin_t origin,
                                    const char *intent, float confidence,
                                    const char *source, size_t *affected);
pm_status_t pin_manager_outputs_all_off(pm_origin_t origin, const char *source,
                                        size_t *affected);
pm_status_t pin_manager_outputs_all_on(bool confirmed, const char *source,
                                       size_t *affected);

pm_status_t pin_manager_get_status(uint8_t pin, pm_pin_status_t *status);
size_t pin_manager_list(pm_pin_status_t *out, size_t capacity, bool only_initialized);
bool pin_manager_resolve_name(const char *name, uint8_t *pin);

pm_status_t pin_manager_create_group(const char *name);
pm_status_t pin_manager_add_group_pin(const char *group, uint8_t pin);
pm_status_t pin_manager_remove_group_pin(const char *group, uint8_t pin);
pm_status_t pin_manager_get_group(const char *name, pm_group_status_t *group);
size_t pin_manager_list_groups(pm_group_status_t *out, size_t capacity);
pm_status_t pin_manager_group_apply(const char *group, bool logical_on, bool confirmed,
                                    pm_origin_t origin, const char *intent,
                                    float confidence, const char *source,
                                    size_t *affected);

pm_status_t pin_manager_validate_pin(uint8_t pin, GPIO_Mode mode, GPIO_Pull pull,
                                     char *reason, size_t reason_size);

/* Versioned configuration payload; the NVS wrapper adds a magic/CRC header. */
size_t pin_manager_export_config(uint8_t *buffer, size_t capacity, pm_status_t *status);
pm_status_t pin_manager_import_config(const uint8_t *buffer, size_t length);

/* Audit log. pin_manager writes state changes; command_engine also records
 * rejected predictions and non-GPIO intents. */
void pin_manager_log_event(pm_origin_t origin, const char *intent, float confidence,
                           int pin, const char *target, const char *source,
                           const char *result);
size_t pin_manager_get_log(pm_action_log_entry_t *out, size_t capacity);
void pin_manager_clear_log(void);

const char *pin_manager_status_name(pm_status_t status);
const char *pin_origin_name(pm_origin_t origin);

/* Non-blocking timed pulse/blink */
typedef void (*pm_blink_notify_fn)(void *context, const char *msg);

pm_status_t pin_manager_blink_start(uint8_t pin, uint32_t duration_ms,
                                    pm_blink_notify_fn notify, void *context);
pm_status_t pin_manager_blink_cancel(pm_blink_notify_fn notify, void *context);
bool pin_manager_blink_is_active(uint8_t *active_pin);
void pin_manager_blink_update(uint32_t now_ms);

#ifdef PIN_MANAGER_HOST_TEST
void pin_manager_test_set_time(uint32_t now_ms);
void pin_manager_test_get_lock_state(uint32_t *lock_count, uint32_t *unlock_count, bool *is_locked);
void pin_manager_test_reset_lock_state(void);
#endif

#ifdef __cplusplus
}
#endif
