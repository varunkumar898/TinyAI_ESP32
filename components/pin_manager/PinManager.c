#include "PinManager.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "board_profile.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static const char *TAG __attribute__((unused)) = "pinmgr";
#define PM_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define PM_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#define PM_LOGE(...) ESP_LOGE(TAG, __VA_ARGS__)
#else
#define PM_LOGI(...) ((void)0)
#define PM_LOGW(...) ((void)0)
#define PM_LOGE(...) ((void)0)
#endif

typedef struct {
    uint8_t pin;
    bool present;
    bool initialized;
    bool configured;
    bool locked;
    bool ai_allowed;
    bool test_approved;
    bool active_high;
    bool input_only;
    bool output_capable;
    uint32_t capabilities;
    GPIO_Mode mode;
    GPIO_Pull pull;
    bool logical_state;
    char device_name[PM_NAME_MAX];
    char aliases[PM_MAX_ALIASES][PM_NAME_MAX];
    uint8_t alias_count;
    char reserved_reason[64];
} pm_record_t;

typedef struct {
    char name[PM_GROUP_NAME_MAX];
    uint64_t pin_mask;
} pm_group_t;

static pm_record_t s_records[PM_MAX_PINS];
static pm_group_t s_groups[PM_MAX_GROUPS];
static size_t s_group_count;
static bool s_initialized;

static pm_action_log_entry_t s_log[PM_LOG_CAPACITY];
static size_t s_log_count;
static size_t s_log_head;
static uint32_t s_log_next_seq = 1;

#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_mutex;
static void pm_lock(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}
static void pm_unlock(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreGive(s_mutex);
    }
}
static uint32_t pm_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
#else
static uint32_t s_host_now_ms;
static void pm_lock(void) {}
static void pm_unlock(void) {}
static uint32_t pm_now_ms(void)
{
    return s_host_now_ms;
}
void pin_manager_test_set_time(uint32_t now_ms)
{
    s_host_now_ms = now_ms;
}
#endif

static void copy_text(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void append_log_locked(pm_origin_t origin, const char *intent, float confidence,
                              int pin, const char *target, const char *source,
                              const char *result)
{
    pm_action_log_entry_t *e = &s_log[s_log_head];
    memset(e, 0, sizeof(*e));
    e->seq = s_log_next_seq++;
    e->timestamp_ms = pm_now_ms();
    e->origin = origin;
    e->pin = (int16_t)pin;
    e->confidence = confidence;
    copy_text(e->intent, sizeof(e->intent), intent);
    copy_text(e->target, sizeof(e->target), target);
    copy_text(e->source, sizeof(e->source), source);
    copy_text(e->result, sizeof(e->result), result);

    s_log_head = (s_log_head + 1) % PM_LOG_CAPACITY;
    if (s_log_count < PM_LOG_CAPACITY) {
        s_log_count++;
    }
}

void pin_manager_log_event(pm_origin_t origin, const char *intent, float confidence,
                           int pin, const char *target, const char *source,
                           const char *result)
{
    pm_lock();
    append_log_locked(origin, intent, confidence, pin, target, source, result);
    pm_unlock();
}

size_t pin_manager_get_log(pm_action_log_entry_t *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    pm_lock();
    size_t copy = s_log_count < capacity ? s_log_count : capacity;
    size_t skip = s_log_count - copy;
    for (size_t i = 0; i < copy; ++i) {
        size_t index = (s_log_head + PM_LOG_CAPACITY - s_log_count + skip + i) %
                       PM_LOG_CAPACITY;
        out[i] = s_log[index];
    }
    pm_unlock();
    return copy;
}

void pin_manager_clear_log(void)
{
    pm_lock();
    s_log_count = 0;
    s_log_head = 0;
    pm_unlock();
}

static bool normalize_name(const char *input, char *out, size_t cap)
{
    if (input == NULL || out == NULL || cap < 2) {
        return false;
    }
    size_t o = 0;
    bool last_space = false;
    bool first = true;
    for (const char *p = input; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) {
            if (!first && o + 1 < cap && !last_space) {
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
        last_space = false;
        first = false;
    }
    if (o == 0) {
        return false;
    }
    if (out[o - 1] == ' ') {
        o--;
    }
    out[o] = '\0';

    static const char *const reserved[] = {
        "all", "devices", "gpio", "group", "help", "none",
        "outputs", "pin", "pins", "status",
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i) {
        if (strcmp(out, reserved[i]) == 0) {
            return false;
        }
    }
    return true;
}

static bool name_in_use_locked(const char *name, int except_pin)
{
    for (size_t i = 0; i < PM_MAX_PINS; ++i) {
        const pm_record_t *r = &s_records[i];
        if (!r->configured || (int)r->pin == except_pin) {
            continue;
        }
        if (strcmp(r->device_name, name) == 0) {
            return true;
        }
        for (uint8_t a = 0; a < r->alias_count; ++a) {
            if (strcmp(r->aliases[a], name) == 0) {
                return true;
            }
        }
    }
    for (size_t g = 0; g < s_group_count; ++g) {
        if (strcmp(s_groups[g].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static int find_group_locked(const char *name)
{
    for (size_t i = 0; i < s_group_count; ++i) {
        if (strcmp(s_groups[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void init_records_locked(void)
{
    memset(s_records, 0, sizeof(s_records));
    memset(s_groups, 0, sizeof(s_groups));
    s_group_count = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
        pm_record_t *r = &s_records[pin];
        r->pin = pin;
        r->active_high = true;
        board_pin_caps_t caps;
        if (board_profile_get_pin_caps(pin, &caps)) {
            r->present = caps.present;
            r->input_only = caps.input_capable && !caps.output_capable;
            r->output_capable = caps.output_capable;
            if (caps.input_capable) {
                r->capabilities |= PM_CAP_INPUT;
            }
            if (caps.output_capable) {
                r->capabilities |= PM_CAP_OUTPUT;
            }
            if (caps.pull_up) {
                r->capabilities |= PM_CAP_PULL_UP;
            }
            if (caps.pull_down) {
                r->capabilities |= PM_CAP_PULL_DOWN;
            }
            if (caps.adc1) {
                r->capabilities |= PM_CAP_ADC1;
            }
            if (caps.adc2) {
                r->capabilities |= PM_CAP_ADC2;
            }
            copy_text(r->reserved_reason, sizeof(r->reserved_reason),
                      caps.reserved ? caps.reserved_reason : "");
        } else {
            copy_text(r->reserved_reason, sizeof(r->reserved_reason),
                      "not present on module");
        }
    }
}

static void fill_status_locked(const pm_record_t *r, pm_pin_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->pin = r->pin;
    status->present = r->present;
    status->initialized = r->initialized;
    status->configured = r->configured;
    status->reserved = r->present && r->reserved_reason[0] != '\0';
    status->locked = r->locked;
    status->ai_allowed = r->ai_allowed;
    status->test_approved = r->test_approved;
    status->active_high = r->active_high;
    status->output_capable = r->output_capable;
    status->input_only = r->input_only;
    status->capabilities = r->capabilities;
    status->mode = r->mode;
    status->pull = r->pull;
    status->logical_state = r->logical_state;
    status->alias_count = r->alias_count;
    copy_text(status->device_name, sizeof(status->device_name), r->device_name);
    for (uint8_t a = 0; a < r->alias_count; ++a) {
        copy_text(status->aliases[a], sizeof(status->aliases[a]), r->aliases[a]);
    }
    copy_text(status->reserved_reason, sizeof(status->reserved_reason),
              r->reserved_reason);
    if (r->initialized) {
        uint8_t level = 0;
        if (GPIO_Read(r->pin, &level) == GPIO_STATUS_OK) {
            status->physical_high = level != 0;
            status->logical_state = r->active_high ? (level != 0) : (level == 0);
        } else {
            status->physical_high = r->logical_state ? r->active_high
                                                     : !r->active_high;
        }
    }
}

static pm_status_t map_gpio_status(GPIO_Status status)
{
    switch (status) {
    case GPIO_STATUS_OK: return PM_OK;
    case GPIO_STATUS_INVALID_PIN: return PM_ERR_INVALID_PIN;
    case GPIO_STATUS_NOT_INITIALIZED: return PM_ERR_UNINITIALIZED;
    case GPIO_STATUS_ALREADY_INITIALIZED: return PM_ERR_ALREADY_INITIALIZED;
    case GPIO_STATUS_UNSUPPORTED_MODE: return PM_ERR_DIRECTION;
    case GPIO_STATUS_UNSUPPORTED_PULL: return PM_ERR_UNSUPPORTED;
    case GPIO_STATUS_BOARD_BLOCKED: return PM_ERR_RESERVED;
    case GPIO_STATUS_INVALID_ARGUMENT: return PM_ERR_INVALID_ARGUMENT;
    case GPIO_STATUS_NOT_OUTPUT: return PM_ERR_DIRECTION;
    default: return PM_ERR_HARDWARE;
    }
}

static pm_status_t validate_board_pin_locked(uint8_t pin, GPIO_Mode mode,
                                             GPIO_Pull pull, char *reason,
                                             size_t reason_size)
{
    if (reason != NULL && reason_size > 0) {
        reason[0] = '\0';
    }
    if (pin >= PM_MAX_PINS) {
        if (reason != NULL && reason_size > 0) {
            copy_text(reason, reason_size, "GPIO must be 0..39");
        }
        return PM_ERR_INVALID_PIN;
    }
    const pm_record_t *r = &s_records[pin];
    if (!r->present) {
        if (reason != NULL && reason_size > 0) {
            copy_text(reason, reason_size, "GPIO is not present on this module");
        }
        return PM_ERR_RESERVED;
    }
    if (r->locked) {
        if (reason != NULL && reason_size > 0) {
            copy_text(reason, reason_size, "pin configuration is locked");
        }
        return PM_ERR_LOCKED;
    }
    const char *why = "";
    GPIO_Status gpio = GPIO_ValidatePin(pin, mode, pull, &why);
    if (gpio != GPIO_STATUS_OK) {
        if (reason != NULL && reason_size > 0) {
            copy_text(reason, reason_size, why);
        }
        return map_gpio_status(gpio);
    }
    return PM_OK;
}

pm_status_t pin_manager_validate_pin(uint8_t pin, GPIO_Mode mode, GPIO_Pull pull,
                                     char *reason, size_t reason_size)
{
    pm_lock();
    pm_status_t status = validate_board_pin_locked(pin, mode, pull, reason, reason_size);
    pm_unlock();
    return status;
}

pm_status_t pin_manager_init(void)
{
#ifdef ESP_PLATFORM
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return PM_ERR_NO_SPACE;
        }
    }
#endif
    pm_lock();
    if (s_initialized) {
        pm_unlock();
        return PM_OK;
    }
    init_records_locked();
    memset(s_log, 0, sizeof(s_log));
    s_log_count = 0;
    s_log_head = 0;
    s_log_next_seq = 1;
    s_initialized = true;
    pm_unlock();
    return PM_OK;
}

static pm_status_t safe_reset_locked(void)
{
    uint64_t set_mask = 0;
    uint64_t clear_mask = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
        pm_record_t *r = &s_records[pin];
        if (!r->initialized || r->mode != GPIO_MODE_OUTPUT) {
            continue;
        }
        bool physical_high = !r->active_high; /* logical OFF */
        if (physical_high) {
            set_mask |= 1ULL << pin;
        } else {
            clear_mask |= 1ULL << pin;
        }
    }
    pm_status_t result = PM_OK;
    if ((set_mask | clear_mask) != 0) {
        GPIO_Status gs = GPIO_WriteMask(set_mask, clear_mask);
        if (gs != GPIO_STATUS_OK) {
            result = map_gpio_status(gs);
        }
    }
    for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
        if (s_records[pin].initialized) {
            (void)GPIO_DeInit(pin);
        }
    }
    init_records_locked();
    return result;
}

pm_status_t pin_manager_reset(void)
{
    pm_lock();
    pm_status_t status = safe_reset_locked();
    append_log_locked(PM_ORIGIN_LOCAL, "reset", 1.0f, -1, "all-pins", "config",
                      status == PM_OK ? "safe-off; config cleared"
                                      : "safe-off best effort; config cleared");
    pm_unlock();
    return status;
}

pm_status_t pin_manager_pin_init(uint8_t pin, GPIO_Mode mode, GPIO_Pull pull)
{
    pm_lock();
    pm_status_t status;
    if (pin >= PM_MAX_PINS) {
        status = PM_ERR_INVALID_PIN;
        goto out;
    }
    pm_record_t *r = &s_records[pin];
    if (r->initialized) {
        status = PM_ERR_ALREADY_INITIALIZED;
        goto out;
    }
    char reason[64];
    status = validate_board_pin_locked(pin, mode, pull, reason, sizeof(reason));
    if (status != PM_OK) {
        append_log_locked(PM_ORIGIN_LOCAL, "pin-init", 1.0f, pin, "gpio", "console",
                          reason);
        goto out;
    }
    GPIO_Status gs = GPIO_InitFull(pin, mode, pull, GPIO_LEVEL_LOW);
    if (gs != GPIO_STATUS_OK) {
        status = map_gpio_status(gs);
        goto out;
    }
    r->initialized = true;
    r->mode = mode;
    r->pull = pull;
    r->logical_state = false;
    r->active_high = true;
    if (mode == GPIO_MODE_INPUT) {
        uint8_t level = 0;
        if (GPIO_Read(pin, &level) == GPIO_STATUS_OK) {
            r->logical_state = level != 0;
        }
    }
    char target[PM_LOG_TARGET_MAX];
    snprintf(target, sizeof(target), "GPIO%u", (unsigned)pin);
    append_log_locked(PM_ORIGIN_LOCAL, "pin-init", 1.0f, pin, target, "console",
                      mode == GPIO_MODE_OUTPUT ? "output; OFF" : "input");
    status = PM_OK;
out:
    pm_unlock();
    return status;
}

static void remove_pin_from_groups_locked(uint8_t pin)
{
    for (size_t g = 0; g < s_group_count; ++g) {
        s_groups[g].pin_mask &= ~(1ULL << pin);
    }
}

static pm_status_t drive_off_locked(pm_record_t *r, const char *intent,
                                    const char *source)
{
    if (!r->initialized || r->mode != GPIO_MODE_OUTPUT) {
        return PM_OK;
    }
    bool physical = !r->active_high;
    GPIO_Status gs = GPIO_Write(r->pin, physical ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
    if (gs != GPIO_STATUS_OK) {
        return map_gpio_status(gs);
    }
    r->logical_state = false;
    char target[PM_LOG_TARGET_MAX];
    if (r->configured) {
        copy_text(target, sizeof(target), r->device_name);
    } else {
        snprintf(target, sizeof(target), "GPIO%u", (unsigned)r->pin);
    }
    append_log_locked(PM_ORIGIN_LOCAL, intent, 1.0f, r->pin, target, source, "OFF");
    return PM_OK;
}

pm_status_t pin_manager_pin_deinit(uint8_t pin)
{
    pm_lock();
    pm_status_t status = PM_OK;
    if (pin >= PM_MAX_PINS) {
        status = PM_ERR_INVALID_PIN;
    } else if (!s_records[pin].initialized) {
        status = PM_ERR_UNINITIALIZED;
    } else if (s_records[pin].locked) {
        status = PM_ERR_LOCKED;
    } else {
        (void)drive_off_locked(&s_records[pin], "deinit", "console");
        remove_pin_from_groups_locked(pin);
        GPIO_Status gs = GPIO_DeInit(pin);
        status = gs == GPIO_STATUS_OK ? PM_OK : map_gpio_status(gs);
        if (status == PM_OK) {
            uint8_t keep = pin;
            /* Rebuild only this record while preserving the audit log. */
            bool present = s_records[pin].present;
            bool output_capable = s_records[pin].output_capable;
            bool input_only = s_records[pin].input_only;
            uint32_t caps = s_records[pin].capabilities;
            char reason[64];
            copy_text(reason, sizeof(reason), s_records[pin].reserved_reason);
            memset(&s_records[pin], 0, sizeof(s_records[pin]));
            s_records[pin].pin = keep;
            s_records[pin].present = present;
            s_records[pin].output_capable = output_capable;
            s_records[pin].input_only = input_only;
            s_records[pin].capabilities = caps;
            s_records[pin].active_high = true;
            copy_text(s_records[pin].reserved_reason,
                      sizeof(s_records[pin].reserved_reason), reason);
            append_log_locked(PM_ORIGIN_LOCAL, "deinit", 1.0f, pin, "gpio", "console",
                              "released");
        }
    }
    pm_unlock();
    return status;
}

pm_status_t pin_manager_assign(uint8_t pin, const char *name, bool active_high,
                               bool ai_allowed, bool test_approved)
{
    if (pin >= PM_MAX_PINS || name == NULL) {
        return PM_ERR_INVALID_ARGUMENT;
    }
    char normalized[PM_NAME_MAX];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return PM_ERR_NAME_INVALID;
    }
    pm_lock();
    pm_record_t *r = &s_records[pin];
    pm_status_t status = PM_OK;
    if (!r->initialized) {
        status = PM_ERR_UNINITIALIZED;
        goto out;
    }
    if (r->configured) {
        status = PM_ERR_ALREADY_CONFIGURED;
        goto out;
    }
    if (r->locked) {
        status = PM_ERR_LOCKED;
        goto out;
    }
    if (r->mode != GPIO_MODE_OUTPUT && test_approved) {
        status = PM_ERR_DIRECTION;
        goto out;
    }
    if (name_in_use_locked(normalized, pin)) {
        status = PM_ERR_NAME_CONFLICT;
        goto out;
    }

    /* If the caller wants active-low polarity, reinitialize the output with
     * the latch already HIGH before enabling the pad. Changing polarity on an
     * already-enabled push-pull output would otherwise create a visible pulse
     * on relay boards. */
    bool previous_polarity = r->active_high;
    if (r->mode == GPIO_MODE_OUTPUT && previous_polarity != active_high) {
        GPIO_Status gs = GPIO_DeInit(pin);
        if (gs != GPIO_STATUS_OK) {
            status = map_gpio_status(gs);
            goto out;
        }
        gs = GPIO_InitFull(pin, GPIO_MODE_OUTPUT, GPIO_PULL_NONE,
                           active_high ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH);
        if (gs != GPIO_STATUS_OK) {
            GPIO_Status restore = GPIO_InitFull(
                pin, GPIO_MODE_OUTPUT, GPIO_PULL_NONE,
                previous_polarity ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH);
            if (restore != GPIO_STATUS_OK) {
                r->initialized = false;
                r->mode = GPIO_MODE_INPUT;
            }
            status = map_gpio_status(gs);
            goto out;
        }
        r->active_high = active_high;
        r->logical_state = false;
    }

    copy_text(r->device_name, sizeof(r->device_name), normalized);
    r->alias_count = 0;
    r->configured = true;
    r->ai_allowed = ai_allowed;
    r->test_approved = test_approved && r->mode == GPIO_MODE_OUTPUT;
    r->active_high = active_high;
    if (r->mode == GPIO_MODE_OUTPUT) {
        status = drive_off_locked(r, "assign", "console");
        if (status != PM_OK) {
            r->configured = false;
            r->device_name[0] = '\0';
            goto out;
        }
    } else {
        uint8_t level = 0;
        if (GPIO_Read(pin, &level) == GPIO_STATUS_OK) {
            r->logical_state = active_high ? (level != 0) : (level == 0);
        }
        append_log_locked(PM_ORIGIN_LOCAL, "assign", 1.0f, pin, normalized, "console",
                          "input");
    }
    status = PM_OK;
out:
    pm_unlock();
    return status;
}

pm_status_t pin_manager_release(uint8_t pin)
{
    if (pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_PIN;
    }
    pm_lock();
    pm_record_t *r = &s_records[pin];
    pm_status_t status = PM_OK;
    if (!r->initialized) {
        status = PM_ERR_UNINITIALIZED;
    } else if (!r->configured) {
        status = PM_ERR_NOT_CONFIGURED;
    } else if (r->locked) {
        status = PM_ERR_LOCKED;
    } else {
        (void)drive_off_locked(r, "release", "console");
        remove_pin_from_groups_locked(pin);
        r->configured = false;
        r->device_name[0] = '\0';
        r->alias_count = 0;
        r->ai_allowed = false;
        r->test_approved = false;
        r->active_high = true;
        append_log_locked(PM_ORIGIN_LOCAL, "release", 1.0f, pin, "gpio", "console",
                          "device removed");
    }
    pm_unlock();
    return status;
}

pm_status_t pin_manager_add_alias(uint8_t pin, const char *alias)
{
    if (pin >= PM_MAX_PINS || alias == NULL) {
        return PM_ERR_INVALID_ARGUMENT;
    }
    char normalized[PM_NAME_MAX];
    if (!normalize_name(alias, normalized, sizeof(normalized))) {
        return PM_ERR_NAME_INVALID;
    }
    pm_lock();
    pm_record_t *r = &s_records[pin];
    pm_status_t status = PM_OK;
    if (!r->configured) {
        status = PM_ERR_NOT_CONFIGURED;
    } else if (r->locked) {
        status = PM_ERR_LOCKED;
    } else if (r->alias_count >= PM_MAX_ALIASES) {
        status = PM_ERR_NO_SPACE;
    } else if (strcmp(normalized, r->device_name) == 0 ||
               name_in_use_locked(normalized, pin)) {
        status = PM_ERR_NAME_CONFLICT;
    } else {
        for (uint8_t i = 0; i < r->alias_count; ++i) {
            if (strcmp(r->aliases[i], normalized) == 0) {
                status = PM_ERR_NAME_CONFLICT;
                goto out;
            }
        }
        copy_text(r->aliases[r->alias_count], PM_NAME_MAX, normalized);
        r->alias_count++;
    }
out:
    pm_unlock();
    return status;
}

pm_status_t pin_manager_set_ai_allowed(uint8_t pin, bool allowed)
{
    if (pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_PIN;
    }
    pm_lock();
    pm_record_t *r = &s_records[pin];
    pm_status_t status = PM_OK;
    if (!r->configured) {
        status = PM_ERR_NOT_CONFIGURED;
    } else if (r->locked) {
        status = PM_ERR_LOCKED;
    } else {
        r->ai_allowed = allowed;
        append_log_locked(PM_ORIGIN_LOCAL, "ai-permission", 1.0f, pin,
                          r->device_name, "console", allowed ? "allowed" : "denied");
    }
    pm_unlock();
    return status;
}

pm_status_t pin_manager_set_test_approved(uint8_t pin, bool approved)
{
    if (pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_PIN;
    }
    pm_lock();
    pm_record_t *r = &s_records[pin];
    pm_status_t status = PM_OK;
    if (!r->configured) {
        status = PM_ERR_NOT_CONFIGURED;
    } else if (r->mode != GPIO_MODE_OUTPUT) {
        status = PM_ERR_DIRECTION;
    } else if (r->locked) {
        status = PM_ERR_LOCKED;
    } else {
        r->test_approved = approved;
        append_log_locked(PM_ORIGIN_LOCAL, "test-approval", 1.0f, pin, r->device_name,
                          "console", approved ? "approved" : "revoked");
    }
    pm_unlock();
    return status;
}

pm_status_t pin_manager_set_locked(uint8_t pin, bool locked)
{
    if (pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_PIN;
    }
    pm_lock();
    pm_record_t *r = &s_records[pin];
    pm_status_t status = PM_OK;
    if (!r->initialized) {
        status = PM_ERR_UNINITIALIZED;
    } else {
        r->locked = locked;
        append_log_locked(PM_ORIGIN_LOCAL, "lock", 1.0f, pin,
                          r->configured ? r->device_name : "gpio", "console",
                          locked ? "locked" : "unlocked");
    }
    pm_unlock();
    return status;
}

static pm_status_t check_control_locked(const pm_record_t *r, pm_origin_t origin)
{
    if (!r->present) {
        return PM_ERR_INVALID_PIN;
    }
    if (!r->initialized) {
        return PM_ERR_UNINITIALIZED;
    }
    if (r->locked) {
        return PM_ERR_LOCKED;
    }
    if (r->mode != GPIO_MODE_OUTPUT) {
        return PM_ERR_DIRECTION;
    }
    if (origin == PM_ORIGIN_AI || origin == PM_ORIGIN_REMOTE) {
        if (!r->configured) {
            return PM_ERR_NOT_CONFIGURED;
        }
        if (!r->ai_allowed) {
            return PM_ERR_AI_DENIED;
        }
    } else if (origin == PM_ORIGIN_AUTOMATION) {
        if (!r->configured) {
            return PM_ERR_NOT_CONFIGURED;
        }
    }
    return PM_OK;
}

static void log_target_for_pin(const pm_record_t *r, char *out, size_t cap)
{
    if (r->configured) {
        copy_text(out, cap, r->device_name);
    } else {
        snprintf(out, cap, "GPIO%u", (unsigned)r->pin);
    }
}

static pm_status_t output_many_locked(const uint8_t *pins, size_t count, bool logical_on,
                                      bool confirmed, pm_origin_t origin,
                                      const char *intent, float confidence,
                                      const char *source, size_t *affected)
{
    if (affected != NULL) {
        *affected = 0;
    }
    if (pins == NULL || count == 0 || count > PM_MAX_PINS) {
        return PM_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < count; ++i) {
        if (pins[i] >= PM_MAX_PINS) {
            return PM_ERR_INVALID_PIN;
        }
        for (size_t j = 0; j < i; ++j) {
            if (pins[i] == pins[j]) {
                return PM_ERR_INVALID_ARGUMENT;
            }
        }
        pm_status_t s = check_control_locked(&s_records[pins[i]], origin);
        if (s != PM_OK) {
            return s;
        }
    }
    if (logical_on && count > 1 && !confirmed) {
        return PM_ERR_CONFIRMATION;
    }

    uint64_t set_mask = 0;
    uint64_t clear_mask = 0;
    for (size_t i = 0; i < count; ++i) {
        const pm_record_t *r = &s_records[pins[i]];
        bool physical_high = r->active_high ? logical_on : !logical_on;
        if (physical_high) {
            set_mask |= 1ULL << r->pin;
        } else {
            clear_mask |= 1ULL << r->pin;
        }
    }
    GPIO_Status gs = GPIO_WriteMask(set_mask, clear_mask);
    if (gs != GPIO_STATUS_OK) {
        return map_gpio_status(gs);
    }
    for (size_t i = 0; i < count; ++i) {
        pm_record_t *r = &s_records[pins[i]];
        r->logical_state = logical_on;
        char target[PM_LOG_TARGET_MAX];
        log_target_for_pin(r, target, sizeof(target));
        append_log_locked(origin, intent, confidence, r->pin, target, source,
                          logical_on ? "ON" : "OFF");
    }
    if (affected != NULL) {
        *affected = count;
    }
    return PM_OK;
}

pm_status_t pin_manager_output(uint8_t pin, bool logical_on, pm_origin_t origin,
                               const char *intent, float confidence,
                               const char *source)
{
    if (pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_PIN;
    }
    pm_lock();
    pm_status_t status = output_many_locked(&pin, 1, logical_on, false, origin,
                                            intent, confidence, source, NULL);
    pm_unlock();
    return status;
}

pm_status_t pin_manager_toggle(uint8_t pin, pm_origin_t origin, const char *intent,
                               float confidence, const char *source)
{
    if (pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_PIN;
    }
    pm_lock();
    pm_record_t *r = &s_records[pin];
    pm_status_t status = check_control_locked(r, origin);
    if (status == PM_OK) {
        bool next = !r->logical_state;
        status = output_many_locked(&pin, 1, next, false, origin, intent, confidence,
                                    source, NULL);
    }
    pm_unlock();
    return status;
}

pm_status_t pin_manager_read(uint8_t pin, bool *physical_high, bool *logical_on)
{
    if (pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_PIN;
    }
    pm_lock();
    pm_record_t *r = &s_records[pin];
    pm_status_t status = PM_OK;
    if (!r->initialized) {
        status = PM_ERR_UNINITIALIZED;
    } else {
        uint8_t value = 0;
        GPIO_Status gs = GPIO_Read(pin, &value);
        if (gs != GPIO_STATUS_OK) {
            status = map_gpio_status(gs);
        } else {
            bool physical = value != 0;
            if (physical_high != NULL) {
                *physical_high = physical;
            }
            if (logical_on != NULL) {
                *logical_on = r->active_high ? physical : !physical;
            }
        }
    }
    pm_unlock();
    return status;
}

pm_status_t pin_manager_output_many(const uint8_t *pins, size_t count, bool logical_on,
                                    bool confirmed, pm_origin_t origin,
                                    const char *intent, float confidence,
                                    const char *source, size_t *affected)
{
    pm_lock();
    pm_status_t status = output_many_locked(pins, count, logical_on, confirmed, origin,
                                            intent, confidence, source, affected);
    pm_unlock();
    return status;
}

static size_t collect_configured_outputs_locked(pm_record_t **out, size_t cap,
                                                bool require_test_approved)
{
    size_t n = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS && n < cap; ++pin) {
        pm_record_t *r = &s_records[pin];
        if (!r->initialized || !r->configured || r->mode != GPIO_MODE_OUTPUT) {
            continue;
        }
        if (r->locked) {
            continue;
        }
        if (require_test_approved && !r->test_approved) {
            continue;
        }
        out[n++] = r;
    }
    return n;
}

pm_status_t pin_manager_outputs_all_off(pm_origin_t origin, const char *source,
                                        size_t *affected)
{
    if (affected != NULL) {
        *affected = 0;
    }
    /* Bulk all-outputs control is a trusted local operation. AI uses an
     * enumerated group/plan that is re-validated by output_many_locked(). */
    if (origin != PM_ORIGIN_LOCAL) {
        return PM_ERR_UNSUPPORTED;
    }
    pm_lock();
    pm_record_t *records[PM_MAX_PINS];
    size_t count = collect_configured_outputs_locked(records, PM_MAX_PINS, false);
    if (count == 0) {
        pm_unlock();
        return PM_OK;
    }
    uint8_t pins[PM_MAX_PINS];
    for (size_t i = 0; i < count; ++i) {
        pins[i] = records[i]->pin;
    }
    pm_status_t status = output_many_locked(pins, count, false, true, PM_ORIGIN_LOCAL,
                                            "outputs-all-off", 1.0f, source, affected);
    pm_unlock();
    return status;
}

pm_status_t pin_manager_outputs_all_on(bool confirmed, const char *source,
                                       size_t *affected)
{
    if (affected != NULL) {
        *affected = 0;
    }
    if (!confirmed) {
        return PM_ERR_CONFIRMATION;
    }
    pm_lock();
    pm_record_t *records[PM_MAX_PINS];
    size_t count = collect_configured_outputs_locked(records, PM_MAX_PINS, true);
    if (count == 0) {
        pm_unlock();
        return PM_ERR_NOT_CONFIGURED;
    }
    uint8_t pins[PM_MAX_PINS];
    for (size_t i = 0; i < count; ++i) {
        pins[i] = records[i]->pin;
    }
    pm_status_t status = output_many_locked(pins, count, true, true, PM_ORIGIN_LOCAL,
                                            "outputs-approved-on", 1.0f, source,
                                            affected);
    pm_unlock();
    return status;
}

pm_status_t pin_manager_get_status(uint8_t pin, pm_pin_status_t *status)
{
    if (status == NULL) {
        return PM_ERR_INVALID_ARGUMENT;
    }
    if (pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_PIN;
    }
    pm_lock();
    fill_status_locked(&s_records[pin], status);
    pm_unlock();
    return PM_OK;
}

size_t pin_manager_list(pm_pin_status_t *out, size_t capacity, bool only_initialized)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    pm_lock();
    size_t n = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS && n < capacity; ++pin) {
        if (only_initialized && !s_records[pin].initialized) {
            continue;
        }
        fill_status_locked(&s_records[pin], &out[n]);
        n++;
    }
    pm_unlock();
    return n;
}

bool pin_manager_resolve_name(const char *name, uint8_t *pin)
{
    if (name == NULL) {
        return false;
    }
    char normalized[PM_NAME_MAX];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return false;
    }
    pm_lock();
    bool found = false;
    for (uint8_t i = 0; i < PM_MAX_PINS; ++i) {
        const pm_record_t *r = &s_records[i];
        if (!r->configured) {
            continue;
        }
        if (strcmp(r->device_name, normalized) == 0) {
            if (pin != NULL) {
                *pin = i;
            }
            found = true;
            break;
        }
        for (uint8_t a = 0; a < r->alias_count; ++a) {
            if (strcmp(r->aliases[a], normalized) == 0) {
                if (pin != NULL) {
                    *pin = i;
                }
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    pm_unlock();
    return found;
}

pm_status_t pin_manager_create_group(const char *name)
{
    if (name == NULL) {
        return PM_ERR_INVALID_ARGUMENT;
    }
    char normalized[PM_GROUP_NAME_MAX];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return PM_ERR_NAME_INVALID;
    }
    pm_lock();
    pm_status_t status = PM_OK;
    if (s_group_count >= PM_MAX_GROUPS) {
        status = PM_ERR_NO_SPACE;
    } else if (find_group_locked(normalized) >= 0) {
        status = PM_ERR_NAME_CONFLICT;
    } else if (name_in_use_locked(normalized, -1)) {
        status = PM_ERR_NAME_CONFLICT;
    } else {
        copy_text(s_groups[s_group_count].name,
                  sizeof(s_groups[s_group_count].name), normalized);
        s_groups[s_group_count].pin_mask = 0;
        s_group_count++;
        append_log_locked(PM_ORIGIN_LOCAL, "group-create", 1.0f, -1, normalized,
                          "console", "created");
    }
    pm_unlock();
    return status;
}

pm_status_t pin_manager_add_group_pin(const char *group, uint8_t pin)
{
    if (group == NULL || pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_ARGUMENT;
    }
    char normalized[PM_GROUP_NAME_MAX];
    if (!normalize_name(group, normalized, sizeof(normalized))) {
        return PM_ERR_NAME_INVALID;
    }
    pm_lock();
    int gi = find_group_locked(normalized);
    pm_status_t status = PM_OK;
    pm_record_t *r = &s_records[pin];
    if (gi < 0) {
        status = PM_ERR_NOT_FOUND;
    } else if (!r->present) {
        status = PM_ERR_INVALID_PIN;
    } else if (!r->initialized || !r->configured) {
        status = PM_ERR_NOT_CONFIGURED;
    } else if (r->mode != GPIO_MODE_OUTPUT) {
        status = PM_ERR_DIRECTION;
    } else if (r->locked) {
        status = PM_ERR_LOCKED;
    } else {
        s_groups[gi].pin_mask |= 1ULL << pin;
        append_log_locked(PM_ORIGIN_LOCAL, "group-add", 1.0f, pin, r->device_name,
                          normalized, "added");
    }
    pm_unlock();
    return status;
}

pm_status_t pin_manager_remove_group_pin(const char *group, uint8_t pin)
{
    if (group == NULL || pin >= PM_MAX_PINS) {
        return PM_ERR_INVALID_ARGUMENT;
    }
    char normalized[PM_GROUP_NAME_MAX];
    if (!normalize_name(group, normalized, sizeof(normalized))) {
        return PM_ERR_NAME_INVALID;
    }
    pm_lock();
    int gi = find_group_locked(normalized);
    pm_status_t status = PM_OK;
    if (gi < 0) {
        status = PM_ERR_NOT_FOUND;
    } else if ((s_groups[gi].pin_mask & (1ULL << pin)) == 0) {
        status = PM_ERR_NOT_FOUND;
    } else if (s_records[pin].locked) {
        status = PM_ERR_LOCKED;
    } else {
        s_groups[gi].pin_mask &= ~(1ULL << pin);
        append_log_locked(PM_ORIGIN_LOCAL, "group-remove", 1.0f, pin, "gpio",
                          normalized, "removed");
    }
    pm_unlock();
    return status;
}

pm_status_t pin_manager_get_group(const char *name, pm_group_status_t *group)
{
    if (name == NULL || group == NULL) {
        return PM_ERR_INVALID_ARGUMENT;
    }
    char normalized[PM_GROUP_NAME_MAX];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return PM_ERR_NAME_INVALID;
    }
    pm_lock();
    int gi = find_group_locked(normalized);
    pm_status_t status = PM_OK;
    if (gi < 0) {
        status = PM_ERR_NOT_FOUND;
    } else {
        memset(group, 0, sizeof(*group));
        copy_text(group->name, sizeof(group->name), s_groups[gi].name);
        group->pin_mask = s_groups[gi].pin_mask;
        for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
            if ((group->pin_mask & (1ULL << pin)) != 0) {
                group->pin_count++;
            }
        }
    }
    pm_unlock();
    return status;
}

size_t pin_manager_list_groups(pm_group_status_t *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    pm_lock();
    size_t n = 0;
    for (size_t i = 0; i < s_group_count && n < capacity; ++i) {
        memset(&out[n], 0, sizeof(out[n]));
        copy_text(out[n].name, sizeof(out[n].name), s_groups[i].name);
        out[n].pin_mask = s_groups[i].pin_mask;
        for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
            if ((out[n].pin_mask & (1ULL << pin)) != 0) {
                out[n].pin_count++;
            }
        }
        n++;
    }
    pm_unlock();
    return n;
}

pm_status_t pin_manager_group_apply(const char *group, bool logical_on, bool confirmed,
                                    pm_origin_t origin, const char *intent,
                                    float confidence, const char *source,
                                    size_t *affected)
{
    if (group == NULL) {
        return PM_ERR_INVALID_ARGUMENT;
    }
    char normalized[PM_GROUP_NAME_MAX];
    if (!normalize_name(group, normalized, sizeof(normalized))) {
        return PM_ERR_NAME_INVALID;
    }
    pm_lock();
    int gi = find_group_locked(normalized);
    if (gi < 0) {
        pm_unlock();
        return PM_ERR_NOT_FOUND;
    }
    uint8_t pins[PM_MAX_PINS];
    size_t count = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
        if ((s_groups[gi].pin_mask & (1ULL << pin)) != 0) {
            pins[count++] = pin;
        }
    }
    if (count == 0) {
        pm_unlock();
        return PM_ERR_NOT_CONFIGURED;
    }
    pm_status_t status = output_many_locked(pins, count, logical_on, confirmed, origin,
                                            intent, confidence, source, affected);
    pm_unlock();
    return status;
}

/* ------------------------------------------------------------------------- */
/* Versioned configuration serialization                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t length;
    bool discard;
    bool overflow;
} writer_t;

static void w_bytes(writer_t *w, const void *data, size_t n)
{
    if (!w->discard && w->length + n <= w->capacity) {
        memcpy(w->buffer + w->length, data, n);
    } else if (!w->discard) {
        w->overflow = true;
    }
    w->length += n;
}

static void w_u8(writer_t *w, uint8_t v)
{
    w_bytes(w, &v, 1);
}

static void w_u16(writer_t *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff)};
    w_bytes(w, b, sizeof(b));
}

static void w_u64(writer_t *w, uint64_t v)
{
    uint8_t b[8];
    for (uint8_t i = 0; i < 8; ++i) {
        b[i] = (uint8_t)((v >> (8 * i)) & 0xff);
    }
    w_bytes(w, b, sizeof(b));
}

typedef struct {
    const uint8_t *buffer;
    size_t length;
    size_t position;
    bool bad;
} reader_t;

static const uint8_t *r_bytes(reader_t *r, size_t n)
{
    if (r->bad || r->position + n > r->length) {
        r->bad = true;
        return NULL;
    }
    const uint8_t *p = r->buffer + r->position;
    r->position += n;
    return p;
}

static uint8_t r_u8(reader_t *r)
{
    const uint8_t *p = r_bytes(r, 1);
    return p != NULL ? p[0] : 0;
}

static uint16_t r_u16(reader_t *r)
{
    const uint8_t *p = r_bytes(r, 2);
    return p != NULL ? (uint16_t)(p[0] | ((uint16_t)p[1] << 8)) : 0;
}

static uint64_t r_u64(reader_t *r)
{
    const uint8_t *p = r_bytes(r, 8);
    if (p == NULL) {
        return 0;
    }
    uint64_t v = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

size_t pin_manager_export_config(uint8_t *buffer, size_t capacity, pm_status_t *status)
{
    writer_t w = {
        .buffer = buffer,
        .capacity = capacity,
        .length = 0,
        .discard = buffer == NULL || capacity == 0,
        .overflow = false,
    };
    pm_lock();
    w_u16(&w, PM_CONFIG_SCHEMA_VERSION);
    uint8_t pin_count = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
        if (s_records[pin].initialized) {
            pin_count++;
        }
    }
    w_u8(&w, pin_count);
    for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
        const pm_record_t *r = &s_records[pin];
        if (!r->initialized) {
            continue;
        }
        w_u8(&w, r->pin);
        w_u8(&w, r->mode == GPIO_MODE_OUTPUT ? 1 : 0);
        w_u8(&w, (uint8_t)r->pull);
        uint8_t flags = 0;
        if (r->active_high) flags |= 0x01;
        if (r->ai_allowed) flags |= 0x02;
        if (r->test_approved) flags |= 0x04;
        if (r->locked) flags |= 0x08;
        if (r->configured) flags |= 0x10;
        w_u8(&w, flags);
        uint8_t name_len = r->configured ? (uint8_t)strlen(r->device_name) : 0;
        w_u8(&w, name_len);
        if (name_len > 0) {
            w_bytes(&w, r->device_name, name_len);
        }
        w_u8(&w, r->configured ? r->alias_count : 0);
        if (r->configured) {
            for (uint8_t a = 0; a < r->alias_count; ++a) {
                uint8_t len = (uint8_t)strlen(r->aliases[a]);
                w_u8(&w, len);
                w_bytes(&w, r->aliases[a], len);
            }
        }
    }
    w_u8(&w, (uint8_t)s_group_count);
    for (size_t i = 0; i < s_group_count; ++i) {
        uint8_t len = (uint8_t)strlen(s_groups[i].name);
        w_u8(&w, len);
        w_bytes(&w, s_groups[i].name, len);
        w_u64(&w, s_groups[i].pin_mask);
    }
    pm_unlock();
    if (status != NULL) {
        *status = w.overflow ? PM_ERR_NO_SPACE : PM_OK;
    }
    return w.length;
}

typedef struct {
    bool used;
    bool output;
    bool configured;
    bool active_high;
    bool ai_allowed;
    bool test_approved;
    bool locked;
    uint8_t pin;
    GPIO_Pull pull;
    char name[PM_NAME_MAX];
    char aliases[PM_MAX_ALIASES][PM_NAME_MAX];
    uint8_t alias_count;
} import_pin_t;

typedef struct {
    char name[PM_GROUP_NAME_MAX];
    uint64_t mask;
} import_group_t;

static import_pin_t s_import_pins[PM_MAX_PINS];
static import_group_t s_import_groups[PM_MAX_GROUPS];

static bool import_name_conflict(size_t upto, const char *name)
{
    for (size_t i = 0; i < upto; ++i) {
        if (!s_import_pins[i].used || !s_import_pins[i].configured) {
            continue;
        }
        if (strcmp(s_import_pins[i].name, name) == 0) {
            return true;
        }
        for (uint8_t a = 0; a < s_import_pins[i].alias_count; ++a) {
            if (strcmp(s_import_pins[i].aliases[a], name) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool read_import_name(reader_t *r, char *out, size_t cap, bool fixed16)
{
    if (fixed16) {
        const uint8_t *p = r_bytes(r, 16);
        if (p == NULL) {
            return false;
        }
        size_t n = 0;
        while (n < 16 && p[n] != '\0') {
            n++;
        }
        if (n >= cap) {
            return false;
        }
        memcpy(out, p, n);
        out[n] = '\0';
    } else {
        uint8_t len = r_u8(r);
        if (len == 0 || len >= cap) {
            return false;
        }
        const uint8_t *p = r_bytes(r, len);
        if (p == NULL) {
            return false;
        }
        memcpy(out, p, len);
        out[len] = '\0';
    }
    return !r->bad;
}

pm_status_t pin_manager_import_config(const uint8_t *buffer, size_t length)
{
    if (buffer == NULL || length < 4) {
        return PM_ERR_BAD_CONFIG;
    }
    memset(s_import_pins, 0, sizeof(s_import_pins));
    memset(s_import_groups, 0, sizeof(s_import_groups));
    reader_t r = {.buffer = buffer, .length = length, .position = 0, .bad = false};
    uint16_t version = r_u16(&r);
    if (version != 1 && version != PM_CONFIG_SCHEMA_VERSION) {
        return PM_ERR_BAD_CONFIG;
    }
    uint8_t pin_count = r_u8(&r);
    if (pin_count > PM_MAX_PINS) {
        return PM_ERR_BAD_CONFIG;
    }
    for (uint8_t i = 0; i < pin_count && !r.bad; ++i) {
        import_pin_t *p = &s_import_pins[i];
        p->pin = r_u8(&r);
        uint8_t mode = r_u8(&r);
        if (mode > 1 || p->pin >= PM_MAX_PINS) {
            return PM_ERR_BAD_CONFIG;
        }
        for (uint8_t k = 0; k < i; ++k) {
            if (s_import_pins[k].used && s_import_pins[k].pin == p->pin) {
                return PM_ERR_BAD_CONFIG;
            }
        }
        p->used = true;
        p->output = mode == 1;
        if (version == 1) {
            p->pull = GPIO_PULL_NONE;
            uint8_t flags = r_u8(&r);
            p->active_high = (flags & 0x01) != 0;
            p->ai_allowed = (flags & 0x02) != 0;
            p->test_approved = false;
            p->locked = false;
            char raw[16];
            if (!read_import_name(&r, raw, sizeof(raw), true)) {
                return PM_ERR_BAD_CONFIG;
            }
            p->configured = raw[0] != '\0';
            if (p->configured) {
                if (!normalize_name(raw, p->name, sizeof(p->name))) {
                    return PM_ERR_BAD_CONFIG;
                }
            }
        } else {
            uint8_t pull = r_u8(&r);
            if (pull > 2) {
                return PM_ERR_BAD_CONFIG;
            }
            p->pull = (GPIO_Pull)pull;
            uint8_t flags = r_u8(&r);
            p->active_high = (flags & 0x01) != 0;
            p->ai_allowed = (flags & 0x02) != 0;
            p->test_approved = (flags & 0x04) != 0;
            p->locked = (flags & 0x08) != 0;
            p->configured = (flags & 0x10) != 0;
            uint8_t name_len = r_u8(&r);
            if (name_len >= PM_NAME_MAX) {
                return PM_ERR_BAD_CONFIG;
            }
            if (name_len > 0) {
                const uint8_t *name = r_bytes(&r, name_len);
                if (name == NULL) {
                    return PM_ERR_BAD_CONFIG;
                }
                char raw[PM_NAME_MAX];
                memcpy(raw, name, name_len);
                raw[name_len] = '\0';
                if (!normalize_name(raw, p->name, sizeof(p->name))) {
                    return PM_ERR_BAD_CONFIG;
                }
            } else if (p->configured) {
                return PM_ERR_BAD_CONFIG;
            }
            uint8_t alias_count = r_u8(&r);
            if (alias_count > PM_MAX_ALIASES) {
                return PM_ERR_BAD_CONFIG;
            }
            for (uint8_t a = 0; a < alias_count; ++a) {
                uint8_t len = r_u8(&r);
                if (len == 0 || len >= PM_NAME_MAX) {
                    return PM_ERR_BAD_CONFIG;
                }
                const uint8_t *alias = r_bytes(&r, len);
                if (alias == NULL) {
                    return PM_ERR_BAD_CONFIG;
                }
                char raw[PM_NAME_MAX];
                memcpy(raw, alias, len);
                raw[len] = '\0';
                if (!normalize_name(raw, p->aliases[p->alias_count],
                                    sizeof(p->aliases[p->alias_count]))) {
                    return PM_ERR_BAD_CONFIG;
                }
                p->alias_count++;
            }
        }
        if (!p->configured) {
            p->alias_count = 0;
            p->name[0] = '\0';
            p->ai_allowed = false;
            p->test_approved = false;
        }
        if (p->test_approved && !p->output) {
            return PM_ERR_BAD_CONFIG;
        }
        if (p->output && p->pull != GPIO_PULL_NONE) {
            return PM_ERR_BAD_CONFIG;
        }
        char reason[64];
        pm_status_t vs = validate_board_pin_locked(
            p->pin, p->output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT, p->pull, reason,
            sizeof(reason));
        bool reserved_ok = vs == PM_ERR_LOCKED; /* current runtime lock is irrelevant */
        if (vs != PM_OK && !reserved_ok) {
            return PM_ERR_BAD_CONFIG;
        }
        if (p->configured) {
            if (import_name_conflict(i, p->name)) {
                return PM_ERR_BAD_CONFIG;
            }
            for (uint8_t a = 0; a < p->alias_count; ++a) {
                if (strcmp(p->aliases[a], p->name) == 0 ||
                    import_name_conflict(i, p->aliases[a])) {
                    return PM_ERR_BAD_CONFIG;
                }
                for (uint8_t b = 0; b < a; ++b) {
                    if (strcmp(p->aliases[a], p->aliases[b]) == 0) {
                        return PM_ERR_BAD_CONFIG;
                    }
                }
            }
        }
    }
    if (r.bad) {
        return PM_ERR_BAD_CONFIG;
    }
    uint8_t group_count = r_u8(&r);
    if (r.bad || group_count > PM_MAX_GROUPS) {
        return PM_ERR_BAD_CONFIG;
    }
    for (uint8_t i = 0; i < group_count && !r.bad; ++i) {
        char raw[PM_GROUP_NAME_MAX];
        if (version == 1) {
            char fixed[16];
            if (!read_import_name(&r, fixed, sizeof(fixed), true)) {
                return PM_ERR_BAD_CONFIG;
            }
            copy_text(raw, sizeof(raw), fixed);
        } else {
            uint8_t len = r_u8(&r);
            if (len == 0 || len >= PM_GROUP_NAME_MAX) {
                return PM_ERR_BAD_CONFIG;
            }
            const uint8_t *p = r_bytes(&r, len);
            if (p == NULL) {
                return PM_ERR_BAD_CONFIG;
            }
            memcpy(raw, p, len);
            raw[len] = '\0';
        }
        if (!normalize_name(raw, s_import_groups[i].name,
                            sizeof(s_import_groups[i].name))) {
            return PM_ERR_BAD_CONFIG;
        }
        s_import_groups[i].mask = version == 1 ? (uint64_t)r_u64(&r) : r_u64(&r);
        if (version == 1) {
            /* v1 stored a 32-bit mask inside a uint64 encoding; the helper
             * above consumed 8 bytes. Legacy v1 is defined as 8 bytes to keep
             * the migration decoder simple and unambiguous. */
        }
        if ((s_import_groups[i].mask & ~((1ULL << 40) - 1ULL)) != 0) {
            return PM_ERR_BAD_CONFIG;
        }
        for (uint8_t j = 0; j < i; ++j) {
            if (strcmp(s_import_groups[i].name, s_import_groups[j].name) == 0) {
                return PM_ERR_BAD_CONFIG;
            }
        }
        for (uint8_t p = 0; p < PM_MAX_PINS; ++p) {
            if (s_import_pins[p].used && s_import_pins[p].configured &&
                (strcmp(s_import_groups[i].name, s_import_pins[p].name) == 0)) {
                return PM_ERR_BAD_CONFIG;
            }
            for (uint8_t a = 0; a < s_import_pins[p].alias_count; ++a) {
                if (strcmp(s_import_groups[i].name, s_import_pins[p].aliases[a]) == 0) {
                    return PM_ERR_BAD_CONFIG;
                }
            }
        }
    }
    if (r.bad) {
        return PM_ERR_BAD_CONFIG;
    }

    /* Validation complete. Apply only now, replacing the runtime config. */
    pm_lock();
    pm_status_t reset_status = safe_reset_locked();
    if (reset_status != PM_OK) {
        pm_unlock();
        return reset_status;
    }
    for (uint8_t i = 0; i < PM_MAX_PINS; ++i) {
        import_pin_t *p = &s_import_pins[i];
        if (!p->used) {
            continue;
        }
        GPIO_Level initial = (p->output && !p->active_high) ? GPIO_LEVEL_HIGH
                                                             : GPIO_LEVEL_LOW;
        GPIO_Status gs = GPIO_InitFull(p->pin,
                                       p->output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
                                       p->pull, initial);
        if (gs != GPIO_STATUS_OK) {
            (void)safe_reset_locked();
            pm_unlock();
            return map_gpio_status(gs);
        }
        pm_record_t *rec = &s_records[p->pin];
        rec->initialized = true;
        rec->mode = p->output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
        rec->pull = p->pull;
        rec->active_high = p->active_high;
        rec->configured = p->configured;
        rec->ai_allowed = p->ai_allowed;
        rec->test_approved = p->test_approved;
        rec->locked = p->locked;
        rec->logical_state = false;
        if (p->configured) {
            copy_text(rec->device_name, sizeof(rec->device_name), p->name);
            rec->alias_count = p->alias_count;
            for (uint8_t a = 0; a < p->alias_count; ++a) {
                copy_text(rec->aliases[a], sizeof(rec->aliases[a]), p->aliases[a]);
            }
        }
        if (!p->output) {
            uint8_t level = 0;
            if (GPIO_Read(p->pin, &level) == GPIO_STATUS_OK) {
                rec->logical_state = p->active_high ? (level != 0) : (level == 0);
            }
        }
    }
    for (uint8_t i = 0; i < group_count; ++i) {
        bool valid = true;
        for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
            if ((s_import_groups[i].mask & (1ULL << pin)) == 0) {
                continue;
            }
            const pm_record_t *rec = &s_records[pin];
            if (!rec->initialized || !rec->configured ||
                rec->mode != GPIO_MODE_OUTPUT) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            append_log_locked(PM_ORIGIN_LOCAL, "config-import", 1.0f, -1,
                              s_import_groups[i].name, "nvs",
                              "group skipped: invalid member");
            continue;
        }
        copy_text(s_groups[s_group_count].name,
                  sizeof(s_groups[s_group_count].name), s_import_groups[i].name);
        s_groups[s_group_count].pin_mask = s_import_groups[i].mask;
        s_group_count++;
    }
    append_log_locked(PM_ORIGIN_LOCAL, "config-import", 1.0f, -1, "config", "nvs",
                      "validated and applied");
    pm_unlock();
    return PM_OK;
}

const char *pin_manager_status_name(pm_status_t status)
{
    switch (status) {
    case PM_OK: return "ok";
    case PM_ERR_INVALID_ARGUMENT: return "invalid-argument";
    case PM_ERR_INVALID_PIN: return "invalid-pin";
    case PM_ERR_UNINITIALIZED: return "not-initialized";
    case PM_ERR_ALREADY_INITIALIZED: return "already-initialized";
    case PM_ERR_NOT_CONFIGURED: return "not-configured";
    case PM_ERR_ALREADY_CONFIGURED: return "already-configured";
    case PM_ERR_RESERVED: return "reserved";
    case PM_ERR_LOCKED: return "locked";
    case PM_ERR_UNSUPPORTED: return "unsupported";
    case PM_ERR_DIRECTION: return "wrong-direction";
    case PM_ERR_NAME_CONFLICT: return "name-conflict";
    case PM_ERR_NAME_INVALID: return "invalid-name";
    case PM_ERR_NOT_FOUND: return "not-found";
    case PM_ERR_NO_SPACE: return "no-space";
    case PM_ERR_AI_DENIED: return "ai-permission-denied";
    case PM_ERR_CONFIRMATION: return "confirmation-required";
    case PM_ERR_HARDWARE: return "hardware-error";
    case PM_ERR_BAD_CONFIG: return "bad-config";
    case PM_ERR_NOT_IMPLEMENTED: return "not-implemented";
    default: return "unknown";
    }
}

const char *pin_origin_name(pm_origin_t origin)
{
    switch (origin) {
    case PM_ORIGIN_LOCAL: return "local";
    case PM_ORIGIN_AI: return "ai";
    case PM_ORIGIN_AUTOMATION: return "automation";
    case PM_ORIGIN_REMOTE: return "remote";
    default: return "unknown";
    }
}
