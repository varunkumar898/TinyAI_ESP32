#include "Automation.h"

#include <ctype.h>
#include <string.h>

#include "PinManager.h"
#include "SensorManager.h"
#include "button_fsm.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static const char *TAG = "automation";
#define AUTO_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define AUTO_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define AUTO_LOGI(...) ((void)0)
#define AUTO_LOGW(...) ((void)0)
#endif

#define RULE_MAX_THRESHOLD 100000000

typedef struct {
    bool bound;
    uint8_t input_pin;
    uint8_t output_pin;
    bool active_high;
    uint32_t hold_ms;
    bool output_owned;
    bool seen_motion;
    uint32_t last_motion_ms;
    bool active;
} pir_binding_t;

typedef struct {
    bool bound;
    uint8_t input_pin;
    uint8_t output_pin;
    bool active_high;
} button_binding_t;

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
    bool fault_logged;
    int32_t last_value;
    uint32_t last_sample_ms;
} automation_rule_t;

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    bool bad;
} rule_writer_t;

typedef struct {
    const uint8_t *buffer;
    size_t length;
    size_t position;
    bool bad;
} rule_reader_t;

static bool s_initialized;
static bool s_enabled;
static pir_binding_t s_pir;
static button_binding_t s_button;
static pir_fsm_t s_pir_fsm;
static button_fsm_t s_button_fsm;
static automation_rule_t s_rules[AUTOMATION_MAX_RULES];
static size_t s_rule_count;

#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_mutex;

static void automation_lock(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void automation_unlock(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreGive(s_mutex);
    }
}
#else
static void automation_lock(void) {}
static void automation_unlock(void) {}
#endif

static bool elapsed_at_least(uint32_t now_ms, uint32_t start_ms, uint32_t limit_ms)
{
    return (uint32_t)(now_ms - start_ms) >= limit_ms;
}

static bool output_is_usable(uint8_t pin)
{
    pm_pin_status_t status;
    if (pin_manager_get_status(pin, &status) != PM_OK) {
        return false;
    }
    return status.initialized && status.configured && !status.locked &&
           status.mode == GPIO_MODE_OUTPUT;
}

static bool input_is_usable(uint8_t pin)
{
    pm_pin_status_t status;
    if (pin_manager_get_status(pin, &status) != PM_OK) {
        return false;
    }
    return status.initialized && !status.locked && status.mode == GPIO_MODE_INPUT;
}

static bool normalize_name(const char *input, char *out, size_t cap)
{
    if (input == NULL || out == NULL || cap < 2) {
        return false;
    }
    size_t o = 0;
    bool first = true;
    bool last_space = false;
    for (const char *p = input; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) {
            if (!first && !last_space && o + 1 < cap) {
                out[o++] = ' ';
                last_space = true;
            }
            continue;
        }
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
        c = (unsigned char)tolower(c);
        if (first && !isalpha(c)) {
            return false;
        }
        if (o + 1 >= cap) {
            return false;
        }
        out[o++] = (char)c;
        first = false;
        last_space = false;
    }
    if (o == 0) {
        return false;
    }
    out[o] = '\0';
    return true;
}

static void release_pir_output_locked(void)
{
    if (s_pir.bound && s_pir.output_owned) {
        (void)pin_manager_output(s_pir.output_pin, false, PM_ORIGIN_AUTOMATION,
                                 "pir-release", 1.0f, "automation");
    }
    s_pir.output_owned = false;
}

static void forget_pir_output_locked(void)
{
    s_pir.output_owned = false;
}

static void clear_rules_locked(void)
{
    memset(s_rules, 0, sizeof(s_rules));
    s_rule_count = 0;
}

static automation_rule_t *find_rule_locked(const char *name)
{
    for (size_t i = 0; i < s_rule_count; ++i) {
        if (strcmp(s_rules[i].name, name) == 0) {
            return &s_rules[i];
        }
    }
    return NULL;
}

static void set_rule_fault_locked(automation_rule_t *rule, const char *detail)
{
    if (!rule->fault_logged) {
        rule->fault_logged = true;
        AUTO_LOGW("rule '%s' fault: %s", rule->name, detail);
        pin_manager_log_event(PM_ORIGIN_AUTOMATION, "rule", 1.0f,
                              (int)rule->output_pin, rule->name, "automation",
                              detail);
    }
}

static void clear_rule_fault_locked(automation_rule_t *rule)
{
    if (rule->fault_logged) {
        rule->fault_logged = false;
        AUTO_LOGI("rule '%s' recovered", rule->name);
        pin_manager_log_event(PM_ORIGIN_AUTOMATION, "rule", 1.0f,
                              (int)rule->output_pin, rule->name, "automation",
                              "recovered");
    }
}

void automation_init(void)
{
    if (s_initialized) {
        return;
    }
#ifdef ESP_PLATFORM
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
#endif
    memset(&s_pir, 0, sizeof(s_pir));
    memset(&s_button, 0, sizeof(s_button));
    memset(&s_pir_fsm, 0, sizeof(s_pir_fsm));
    memset(&s_button_fsm, 0, sizeof(s_button_fsm));
    clear_rules_locked();
    s_enabled = false;
    s_initialized = true;
}

void automation_clear(void)
{
    automation_lock();
    forget_pir_output_locked();
    memset(&s_pir, 0, sizeof(s_pir));
    memset(&s_button, 0, sizeof(s_button));
    memset(&s_pir_fsm, 0, sizeof(s_pir_fsm));
    memset(&s_button_fsm, 0, sizeof(s_button_fsm));
    clear_rules_locked();
    s_enabled = false;
    automation_unlock();
}

void automation_set_enabled(bool enabled)
{
    automation_lock();
    if (!enabled) {
        forget_pir_output_locked();
        s_pir_fsm.initialized = false;
        s_button_fsm.initialized = false;
    } else if (!s_enabled) {
        s_pir_fsm.initialized = false;
        s_button_fsm.initialized = false;
    }
    s_enabled = enabled;
    automation_unlock();
}

bool automation_is_enabled(void)
{
    bool enabled;
    automation_lock();
    enabled = s_enabled;
    automation_unlock();
    return enabled;
}

bool automation_bind_pir(uint8_t input_pin, bool active_high, uint8_t output_pin,
                         uint32_t hold_ms)
{
    if (input_pin == output_pin || !input_is_usable(input_pin) ||
        !output_is_usable(output_pin)) {
        return false;
    }
    if (hold_ms < 50) {
        hold_ms = AUTOMATION_DEFAULT_PIR_HOLD_MS;
    }

    automation_lock();
    release_pir_output_locked();
    memset(&s_pir_fsm, 0, sizeof(s_pir_fsm));
    s_pir.bound = true;
    s_pir.input_pin = input_pin;
    s_pir.output_pin = output_pin;
    s_pir.active_high = active_high;
    s_pir.hold_ms = hold_ms;
    s_pir.output_owned = false;
    s_pir.seen_motion = false;
    s_pir.last_motion_ms = 0;
    s_pir.active = false;
    automation_unlock();

    AUTO_LOGI("PIR GPIO%u active-%s -> GPIO%u hold=%u ms bound",
              (unsigned)input_pin, active_high ? "high" : "low",
              (unsigned)output_pin, (unsigned)hold_ms);
    return true;
}

bool automation_bind_button(uint8_t input_pin, bool active_high,
                            uint8_t output_pin)
{
    if (input_pin == output_pin || !input_is_usable(input_pin) ||
        !output_is_usable(output_pin)) {
        return false;
    }

    automation_lock();
    s_button.bound = true;
    s_button.input_pin = input_pin;
    s_button.output_pin = output_pin;
    s_button.active_high = active_high;
    memset(&s_button_fsm, 0, sizeof(s_button_fsm));
    automation_unlock();

    AUTO_LOGI("button GPIO%u active-%s -> GPIO%u bound", (unsigned)input_pin,
              active_high ? "high" : "low", (unsigned)output_pin);
    return true;
}

automation_rule_status_t automation_rule_add(const char *name, const char *sensor,
                                             uint8_t output_pin,
                                             automation_condition_t condition,
                                             int32_t threshold,
                                             uint32_t hysteresis)
{
    char rule_name[AUTOMATION_RULE_NAME_MAX];
    char sensor_name[SENSOR_NAME_MAX];
    if (!normalize_name(name, rule_name, sizeof(rule_name)) ||
        !normalize_name(sensor, sensor_name, sizeof(sensor_name))) {
        return AUTOMATION_RULE_INVALID_ARGUMENT;
    }
    if (condition != AUTOMATION_ABOVE && condition != AUTOMATION_BELOW) {
        return AUTOMATION_RULE_INVALID_ARGUMENT;
    }
    if (threshold < -RULE_MAX_THRESHOLD || threshold > RULE_MAX_THRESHOLD) {
        return AUTOMATION_RULE_INVALID_ARGUMENT;
    }
    if (hysteresis > (uint32_t)RULE_MAX_THRESHOLD) {
        return AUTOMATION_RULE_INVALID_ARGUMENT;
    }
    if (!output_is_usable(output_pin)) {
        return AUTOMATION_RULE_OUTPUT_UNUSABLE;
    }
    sensor_sample_t sample;
    if (sensor_manager_get(sensor_name, &sample) != SENSOR_OK) {
        return AUTOMATION_RULE_SENSOR_MISSING;
    }

    automation_lock();
    if (find_rule_locked(rule_name) != NULL) {
        automation_unlock();
        return AUTOMATION_RULE_NAME_CONFLICT;
    }
    if (s_rule_count >= AUTOMATION_MAX_RULES) {
        automation_unlock();
        return AUTOMATION_RULE_NO_SPACE;
    }
    automation_rule_t *rule = &s_rules[s_rule_count];
    memset(rule, 0, sizeof(*rule));
    strcpy(rule->name, rule_name);
    strcpy(rule->sensor, sensor_name);
    rule->output_pin = output_pin;
    rule->condition = condition;
    rule->threshold = threshold;
    rule->hysteresis = hysteresis;
    rule->enabled = true;
    ++s_rule_count;
    automation_unlock();

    pin_manager_log_event(PM_ORIGIN_LOCAL, "rule-add", 1.0f,
                          (int)output_pin, rule_name, "automation", "added");
    return AUTOMATION_RULE_OK;
}

automation_rule_status_t automation_rule_remove(const char *name)
{
    char rule_name[AUTOMATION_RULE_NAME_MAX];
    if (!normalize_name(name, rule_name, sizeof(rule_name))) {
        return AUTOMATION_RULE_INVALID_ARGUMENT;
    }
    automation_lock();
    for (size_t i = 0; i < s_rule_count; ++i) {
        if (strcmp(s_rules[i].name, rule_name) != 0) {
            continue;
        }
        for (size_t j = i + 1; j < s_rule_count; ++j) {
            s_rules[j - 1] = s_rules[j];
        }
        --s_rule_count;
        memset(&s_rules[s_rule_count], 0, sizeof(s_rules[0]));
        automation_unlock();
        pin_manager_log_event(PM_ORIGIN_LOCAL, "rule-remove", 1.0f, -1,
                              rule_name, "automation", "removed (output unchanged)");
        return AUTOMATION_RULE_OK;
    }
    automation_unlock();
    return AUTOMATION_RULE_NOT_FOUND;
}

automation_rule_status_t automation_rule_set_enabled(const char *name,
                                                     bool enabled)
{
    char rule_name[AUTOMATION_RULE_NAME_MAX];
    if (!normalize_name(name, rule_name, sizeof(rule_name))) {
        return AUTOMATION_RULE_INVALID_ARGUMENT;
    }
    automation_lock();
    automation_rule_t *rule = find_rule_locked(rule_name);
    if (rule == NULL) {
        automation_unlock();
        return AUTOMATION_RULE_NOT_FOUND;
    }
    uint8_t output_pin = rule->output_pin;
    rule->enabled = enabled;
    automation_unlock();
    pin_manager_log_event(PM_ORIGIN_LOCAL, enabled ? "rule-enable" : "rule-pause",
                          1.0f, (int)output_pin, rule_name, "automation",
                          enabled ? "enabled" : "paused (output unchanged)");
    return AUTOMATION_RULE_OK;
}

automation_rule_status_t automation_rule_reset(const char *name)
{
    char rule_name[AUTOMATION_RULE_NAME_MAX];
    if (!normalize_name(name, rule_name, sizeof(rule_name))) {
        return AUTOMATION_RULE_INVALID_ARGUMENT;
    }
    automation_lock();
    automation_rule_t *rule = find_rule_locked(rule_name);
    if (rule == NULL) {
        automation_unlock();
        return AUTOMATION_RULE_NOT_FOUND;
    }
    rule->active = false;
    rule->output_owned = false;
    rule->fault_logged = false;
    rule->last_value = 0;
    rule->last_sample_ms = 0;
    uint8_t output_pin = rule->output_pin;
    automation_unlock();
    pin_manager_log_event(PM_ORIGIN_LOCAL, "rule-reset", 1.0f,
                          (int)output_pin, rule_name, "automation",
                          "reset (output unchanged)");
    return AUTOMATION_RULE_OK;
}

size_t automation_rule_list(automation_rule_info_t *out, size_t capacity)
{
    automation_lock();
    size_t copy = s_rule_count < capacity ? s_rule_count : capacity;
    for (size_t i = 0; i < copy; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].name, s_rules[i].name, sizeof(out[i].name));
        memcpy(out[i].sensor, s_rules[i].sensor, sizeof(out[i].sensor));
        out[i].output_pin = s_rules[i].output_pin;
        out[i].condition = s_rules[i].condition;
        out[i].threshold = s_rules[i].threshold;
        out[i].hysteresis = s_rules[i].hysteresis;
        out[i].enabled = s_rules[i].enabled;
        out[i].active = s_rules[i].active;
        out[i].output_owned = s_rules[i].output_owned;
        out[i].fault = s_rules[i].fault_logged;
        out[i].last_value = s_rules[i].last_value;
        out[i].last_sample_ms = s_rules[i].last_sample_ms;
    }
    automation_unlock();
    return copy;
}

size_t automation_rule_count(void)
{
    size_t count;
    automation_lock();
    count = s_rule_count;
    automation_unlock();
    return count;
}

void automation_get_status(automation_status_t *out)
{
    if (out == NULL) {
        return;
    }
    automation_lock();
    memset(out, 0, sizeof(*out));
    out->enabled = s_enabled;
    out->pir_bound = s_pir.bound;
    out->pir_pin = s_pir.input_pin;
    out->pir_output_pin = s_pir.output_pin;
    out->pir_active_high = s_pir.active_high;
    out->pir_hold_ms = s_pir.hold_ms;
    out->pir_active = s_pir.active;
    out->pir_output_owned = s_pir.output_owned;
    out->button_bound = s_button.bound;
    out->button_pin = s_button.input_pin;
    out->button_output_pin = s_button.output_pin;
    out->button_active_low = !s_button.active_high;
    automation_unlock();
}

static size_t update_pir_locked(uint32_t now_ms)
{
    if (!s_pir.bound) {
        return 0;
    }
    bool physical_high = false;
    bool logical_on = false;
    if (pin_manager_read(s_pir.input_pin, &physical_high, &logical_on) != PM_OK) {
        return 0;
    }
    (void)logical_on;
    bool raw_active = s_pir.active_high ? physical_high : !physical_high;
    bool active = pir_fsm_update(&s_pir_fsm, raw_active, now_ms);
    s_pir.active = active;

    size_t changed = 0;
    if (active) {
        s_pir.seen_motion = true;
        s_pir.last_motion_ms = now_ms;
        if (!s_pir.output_owned) {
            pm_status_t status =
                pin_manager_output(s_pir.output_pin, true, PM_ORIGIN_AUTOMATION,
                                   "pir-on", 1.0f, "automation");
            if (status == PM_OK) {
                s_pir.output_owned = true;
                ++changed;
            }
        }
    } else if (s_pir.seen_motion &&
               elapsed_at_least(now_ms, s_pir.last_motion_ms, s_pir.hold_ms)) {
        if (s_pir.output_owned) {
            pm_status_t status =
                pin_manager_output(s_pir.output_pin, false, PM_ORIGIN_AUTOMATION,
                                   "pir-off", 1.0f, "automation");
            if (status == PM_OK) {
                ++changed;
            }
        }
        s_pir.output_owned = false;
        s_pir.seen_motion = false;
    }
    return changed;
}

static size_t update_button_locked(uint32_t now_ms)
{
    if (!s_button.bound) {
        return 0;
    }
    bool physical_high = false;
    bool logical_on = false;
    if (pin_manager_read(s_button.input_pin, &physical_high, &logical_on) !=
        PM_OK) {
        return 0;
    }
    (void)logical_on;
    bool raw_active = s_button.active_high ? physical_high : !physical_high;
    if (!button_fsm_update(&s_button_fsm, raw_active, now_ms)) {
        return 0;
    }
    pm_status_t status =
        pin_manager_toggle(s_button.output_pin, PM_ORIGIN_AUTOMATION, "button",
                           1.0f, "automation");
    if (status != PM_OK) {
        return 0;
    }
    return 1;
}

static bool rule_next_active(const automation_rule_t *rule, int32_t value)
{
    if (rule->condition == AUTOMATION_ABOVE) {
        int64_t up = rule->threshold;
        int64_t down = (int64_t)rule->threshold - (int64_t)rule->hysteresis;
        if (!rule->active && (int64_t)value >= up) {
            return true;
        }
        if (rule->active && (int64_t)value <= down) {
            return false;
        }
    } else {
        int64_t down = rule->threshold;
        int64_t up = (int64_t)rule->threshold + (int64_t)rule->hysteresis;
        if (!rule->active && (int64_t)value <= down) {
            return true;
        }
        if (rule->active && (int64_t)value >= up) {
            return false;
        }
    }
    return rule->active;
}

static size_t update_rule_locked(automation_rule_t *rule)
{
    if (!rule->enabled) {
        return 0;
    }
    sensor_sample_t sample;
    sensor_status_t sample_status = sensor_manager_sample(rule->sensor, &sample);
    if (sample_status != SENSOR_OK || !sample.valid) {
        set_rule_fault_locked(rule, sample_status == SENSOR_OK
                                        ? "sensor sample invalid"
                                        : "sensor unavailable");
        return 0;
    }

    rule->last_value = sample.value;
    rule->last_sample_ms = sample.last_sample_ms;
    rule->active = rule_next_active(rule, sample.value);

    size_t changed = 0;
    pm_status_t status = PM_OK;
    if (rule->active && !rule->output_owned) {
        status = pin_manager_output(rule->output_pin, true, PM_ORIGIN_AUTOMATION,
                                    "rule-on", 1.0f, "automation");
        if (status == PM_OK) {
            rule->output_owned = true;
            ++changed;
        }
    } else if (!rule->active && rule->output_owned) {
        status = pin_manager_output(rule->output_pin, false,
                                    PM_ORIGIN_AUTOMATION, "rule-off", 1.0f,
                                    "automation");
        if (status == PM_OK) {
            rule->output_owned = false;
            ++changed;
        }
    }

    if (status != PM_OK) {
        set_rule_fault_locked(rule, pin_manager_status_name(status));
    } else {
        clear_rule_fault_locked(rule);
    }
    return changed;
}

size_t automation_update(uint32_t now_ms)
{
    automation_lock();
    size_t changed = 0;
    if (s_enabled) {
        changed += update_pir_locked(now_ms);
        changed += update_button_locked(now_ms);
        for (size_t i = 0; i < s_rule_count; ++i) {
            changed += update_rule_locked(&s_rules[i]);
        }
    }
    automation_unlock();
    return changed;
}

static void writer_put(rule_writer_t *w, const void *data, size_t length)
{
    if (w->bad || length > w->capacity - w->length) {
        w->bad = true;
        return;
    }
    memcpy(w->buffer + w->length, data, length);
    w->length += length;
}

static void writer_u8(rule_writer_t *w, uint8_t value)
{
    writer_put(w, &value, 1);
}

static void writer_u16(rule_writer_t *w, uint16_t value)
{
    uint8_t bytes[2] = {(uint8_t)(value & 0xffu), (uint8_t)(value >> 8)};
    writer_put(w, bytes, sizeof(bytes));
}

static void writer_u32(rule_writer_t *w, uint32_t value)
{
    uint8_t bytes[4] = {(uint8_t)(value & 0xffu), (uint8_t)((value >> 8) & 0xffu),
                        (uint8_t)((value >> 16) & 0xffu),
                        (uint8_t)((value >> 24) & 0xffu)};
    writer_put(w, bytes, sizeof(bytes));
}

static void writer_i32(rule_writer_t *w, int32_t value)
{
    writer_u32(w, (uint32_t)value);
}

static uint8_t reader_u8(rule_reader_t *r)
{
    if (r->bad || r->position >= r->length) {
        r->bad = true;
        return 0;
    }
    return r->buffer[r->position++];
}

static uint16_t reader_u16(rule_reader_t *r)
{
    if (r->bad || r->length - r->position < 2) {
        r->bad = true;
        return 0;
    }
    uint16_t value = (uint16_t)(r->buffer[r->position] |
                                ((uint16_t)r->buffer[r->position + 1] << 8));
    r->position += 2;
    return value;
}

static uint32_t reader_u32(rule_reader_t *r)
{
    if (r->bad || r->length - r->position < 4) {
        r->bad = true;
        return 0;
    }
    uint32_t value = (uint32_t)r->buffer[r->position] |
                     ((uint32_t)r->buffer[r->position + 1] << 8) |
                     ((uint32_t)r->buffer[r->position + 2] << 16) |
                     ((uint32_t)r->buffer[r->position + 3] << 24);
    r->position += 4;
    return value;
}

static int32_t reader_i32(rule_reader_t *r)
{
    return (int32_t)reader_u32(r);
}

static bool reader_name(rule_reader_t *r, char *out, size_t capacity)
{
    uint8_t length = reader_u8(r);
    if (r->bad || length == 0 || length >= capacity ||
        r->length - r->position < length) {
        r->bad = true;
        return false;
    }
    memcpy(out, r->buffer + r->position, length);
    out[length] = '\0';
    r->position += length;
    return true;
}

size_t automation_export_config(uint8_t *buffer, size_t capacity,
                                automation_config_status_t *status)
{
    if (status != NULL) {
        *status = AUTOMATION_CONFIG_BAD_CONFIG;
    }
    if (buffer == NULL || capacity < 3) {
        if (status != NULL) {
            *status = AUTOMATION_CONFIG_NO_SPACE;
        }
        return 0;
    }

    rule_writer_t w = {
        .buffer = buffer,
        .capacity = capacity,
        .length = 0,
        .bad = false,
    };
    automation_lock();
    writer_u16(&w, AUTOMATION_CONFIG_SCHEMA_VERSION);
    writer_u8(&w, (uint8_t)s_rule_count);
    for (size_t i = 0; i < s_rule_count; ++i) {
        const automation_rule_t *rule = &s_rules[i];
        size_t name_len = strlen(rule->name);
        size_t sensor_len = strlen(rule->sensor);
        writer_u8(&w, (uint8_t)name_len);
        writer_put(&w, rule->name, name_len);
        writer_u8(&w, (uint8_t)sensor_len);
        writer_put(&w, rule->sensor, sensor_len);
        writer_u8(&w, rule->output_pin);
        writer_u8(&w, (uint8_t)rule->condition);
        writer_i32(&w, rule->threshold);
        writer_u32(&w, rule->hysteresis);
        writer_u8(&w, rule->enabled ? 1 : 0);
    }
    automation_unlock();

    if (w.bad) {
        if (status != NULL) {
            *status = AUTOMATION_CONFIG_NO_SPACE;
        }
        return 0;
    }
    if (status != NULL) {
        *status = AUTOMATION_CONFIG_OK;
    }
    return w.length;
}

automation_config_status_t automation_import_config(const uint8_t *buffer,
                                                    size_t length)
{
    if (buffer == NULL || length < 3) {
        return AUTOMATION_CONFIG_BAD_CONFIG;
    }
    rule_reader_t r = {
        .buffer = buffer,
        .length = length,
        .position = 0,
        .bad = false,
    };
    if (reader_u16(&r) != AUTOMATION_CONFIG_SCHEMA_VERSION) {
        return AUTOMATION_CONFIG_BAD_CONFIG;
    }
    uint8_t count = reader_u8(&r);
    if (r.bad || count > AUTOMATION_MAX_RULES) {
        return AUTOMATION_CONFIG_BAD_CONFIG;
    }

    automation_rule_t parsed[AUTOMATION_MAX_RULES];
    memset(parsed, 0, sizeof(parsed));
    for (size_t i = 0; i < count; ++i) {
        if (!reader_name(&r, parsed[i].name, sizeof(parsed[i].name)) ||
            !reader_name(&r, parsed[i].sensor, sizeof(parsed[i].sensor))) {
            return AUTOMATION_CONFIG_BAD_CONFIG;
        }
        parsed[i].output_pin = reader_u8(&r);
        uint8_t condition = reader_u8(&r);
        parsed[i].threshold = reader_i32(&r);
        parsed[i].hysteresis = reader_u32(&r);
        uint8_t enabled = reader_u8(&r);
        if (r.bad || parsed[i].output_pin >= PM_MAX_PINS ||
            condition > (uint8_t)AUTOMATION_BELOW || enabled > 1) {
            return AUTOMATION_CONFIG_BAD_CONFIG;
        }
        parsed[i].condition = (automation_condition_t)condition;
        parsed[i].enabled = enabled != 0;
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(parsed[j].name, parsed[i].name) == 0) {
                return AUTOMATION_CONFIG_BAD_CONFIG;
            }
        }
    }
    if (r.bad || r.position != length) {
        return AUTOMATION_CONFIG_BAD_CONFIG;
    }

    automation_lock();
    for (size_t i = 0; i < count; ++i) {
        sensor_sample_t sample;
        if (sensor_manager_get(parsed[i].sensor, &sample) != SENSOR_OK ||
            !output_is_usable(parsed[i].output_pin)) {
            automation_unlock();
            return AUTOMATION_CONFIG_BAD_CONFIG;
        }
    }
    clear_rules_locked();
    memcpy(s_rules, parsed, (size_t)count * sizeof(parsed[0]));
    s_rule_count = count;
    automation_unlock();
    return AUTOMATION_CONFIG_OK;
}

const char *automation_rule_status_name(automation_rule_status_t status)
{
    switch (status) {
    case AUTOMATION_RULE_OK: return "ok";
    case AUTOMATION_RULE_INVALID_ARGUMENT: return "invalid-argument";
    case AUTOMATION_RULE_NAME_CONFLICT: return "name-conflict";
    case AUTOMATION_RULE_NO_SPACE: return "no-space";
    case AUTOMATION_RULE_NOT_FOUND: return "not-found";
    case AUTOMATION_RULE_SENSOR_MISSING: return "sensor-missing";
    case AUTOMATION_RULE_OUTPUT_UNUSABLE: return "output-unusable";
    default: return "unknown";
    }
}
