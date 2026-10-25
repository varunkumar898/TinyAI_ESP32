#include "SensorManager.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static const char *TAG = "sensor";
#define SENSOR_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define SENSOR_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define SENSOR_LOGI(...) ((void)0)
#define SENSOR_LOGW(...) ((void)0)
#endif

typedef struct {
    char name[SENSOR_NAME_MAX];
    sensor_kind_t kind;
    uint8_t pin;
    uint8_t adc_channel;
    bool valid;
    uint32_t last_sample_ms;
    int32_t raw;
    int32_t value;
} sensor_record_t;

static sensor_record_t s_sensors[SENSOR_MAX];
static size_t s_sensor_count;
static bool s_initialized;

#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_mutex;
static adc_oneshot_unit_handle_t s_adc_unit;

static void sensor_lock(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void sensor_unlock(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreGive(s_mutex);
    }
}

static uint32_t sensor_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
#else
static uint32_t s_host_now_ms;
static uint16_t s_host_adc_raw[PM_MAX_PINS];

static void sensor_lock(void) {}
static void sensor_unlock(void) {}

static uint32_t sensor_now_ms(void)
{
    return s_host_now_ms;
}

void sensor_manager_test_set_adc_raw(uint8_t pin, uint16_t raw)
{
    if (pin < PM_MAX_PINS) {
        s_host_adc_raw[pin] = raw > 4095u ? 4095u : raw;
    }
}

void sensor_manager_test_set_time(uint32_t now_ms)
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

static int find_sensor_locked(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    char normalized[SENSOR_NAME_MAX];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return -1;
    }
    for (size_t i = 0; i < s_sensor_count; ++i) {
        if (strcmp(s_sensors[i].name, normalized) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void fill_status_locked(const sensor_record_t *r, sensor_sample_t *out)
{
    memset(out, 0, sizeof(*out));
    copy_text(out->name, sizeof(out->name), r->name);
    out->kind = r->kind;
    out->pin = r->pin;
    out->valid = r->valid;
    out->last_sample_ms = r->last_sample_ms;
    out->raw = r->raw;
    out->value = r->value;
}

static uint8_t adc1_channel_for_pin(uint8_t pin)
{
    switch (pin) {
    case 36: return 0;
    case 37: return 1;
    case 38: return 2;
    case 39: return 3;
    case 32: return 4;
    case 33: return 5;
    case 34: return 6;
    case 35: return 7;
    default: return 0xff;
    }
}

#ifdef ESP_PLATFORM
static bool adc_ensure_unit(void)
{
    if (s_adc_unit != NULL) {
        return true;
    }
    adc_oneshot_unit_init_cfg_t cfg = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&cfg, &s_adc_unit) != ESP_OK) {
        s_adc_unit = NULL;
        return false;
    }
    return true;
}

static bool adc_configure_channel(uint8_t channel)
{
    if (!adc_ensure_unit()) {
        return false;
    }
    adc_oneshot_chan_cfg_t cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    return adc_oneshot_config_channel(s_adc_unit, (adc_channel_t)channel, &cfg) ==
           ESP_OK;
}

static void adc_release_unit(void)
{
    if (s_adc_unit != NULL) {
        (void)adc_oneshot_del_unit(s_adc_unit);
        s_adc_unit = NULL;
    }
}
#else
static bool adc_ensure_unit(void)
{
    return true;
}

static bool adc_configure_channel(uint8_t channel)
{
    (void)channel;
    return true;
}

static void adc_release_unit(void) {}
#endif

static sensor_status_t validate_pin_for_kind_locked(uint8_t pin, sensor_kind_t kind,
                                                     char *reason, size_t reason_size)
{
    if (reason != NULL && reason_size > 0) {
        reason[0] = '\0';
    }
    pm_pin_status_t status;
    pm_status_t pm = pin_manager_get_status(pin, &status);
    if (pm != PM_OK) {
        if (reason != NULL && reason_size > 0) {
            copy_text(reason, reason_size, pin_manager_status_name(pm));
        }
        return SENSOR_ERR_PIN_NOT_READY;
    }
    if (!status.initialized) {
        if (reason != NULL && reason_size > 0) {
            copy_text(reason, reason_size, "pin is not initialized");
        }
        return SENSOR_ERR_PIN_NOT_READY;
    }
    if (status.mode == GPIO_MODE_OUTPUT) {
        if (reason != NULL && reason_size > 0) {
            copy_text(reason, reason_size, "pin is an output");
        }
        return SENSOR_ERR_NOT_INPUT;
    }
    if (kind == SENSOR_KIND_ADC1) {
        if ((status.capabilities & PM_CAP_ADC1) == 0 ||
            adc1_channel_for_pin(pin) == 0xff) {
            if (reason != NULL && reason_size > 0) {
                copy_text(reason, reason_size, "pin is not an ESP32 ADC1 pin");
            }
            return SENSOR_ERR_NOT_ADC1;
        }
        if (status.mode != GPIO_MODE_INPUT || status.pull != GPIO_PULL_NONE) {
            if (reason != NULL && reason_size > 0) {
                copy_text(reason, reason_size,
                          "ADC input must be GPIO_MODE_INPUT with no pull");
            }
            return SENSOR_ERR_ADC_USES_PULL;
        }
    }
    return SENSOR_OK;
}

static void sample_record_locked(sensor_record_t *r)
{
    r->last_sample_ms = sensor_now_ms();
    if (r->kind == SENSOR_KIND_DIGITAL) {
        bool physical = false;
        bool logical = false;
        pm_status_t pm = pin_manager_read(r->pin, &physical, &logical);
        (void)physical;
        if (pm != PM_OK) {
            r->valid = false;
            return;
        }
        r->valid = true;
        r->raw = logical ? 1 : 0;
        r->value = r->raw;
        return;
    }

#ifdef ESP_PLATFORM
    int raw = 0;
    if (s_adc_unit == NULL ||
        adc_oneshot_read(s_adc_unit, (adc_channel_t)r->adc_channel, &raw) != ESP_OK) {
        r->valid = false;
        return;
    }
#else
    int raw = (int)s_host_adc_raw[r->pin];
#endif
    if (raw < 0) {
        raw = 0;
    }
    if (raw > 4095) {
        raw = 4095;
    }
    r->valid = true;
    r->raw = raw;
    /* 12 dB attenuation on the classic ESP32 is roughly 0..1.1 V. This is an
     * uncalibrated conversion; the CLI labels it as approximate. */
    r->value = (raw * 1100) / 4095;
}

void sensor_manager_init(void)
{
#ifdef ESP_PLATFORM
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
#endif
    sensor_lock();
    if (!s_initialized) {
        memset(s_sensors, 0, sizeof(s_sensors));
        s_sensor_count = 0;
        s_initialized = true;
    }
    sensor_unlock();
}

sensor_status_t sensor_manager_reset(void)
{
    sensor_lock();
    memset(s_sensors, 0, sizeof(s_sensors));
    s_sensor_count = 0;
    s_initialized = true;
    adc_release_unit();
    sensor_unlock();
    return SENSOR_OK;
}

sensor_status_t sensor_manager_add(const char *name, sensor_kind_t kind, uint8_t pin)
{
    if (name == NULL || (kind != SENSOR_KIND_DIGITAL && kind != SENSOR_KIND_ADC1)) {
        return SENSOR_ERR_INVALID_ARGUMENT;
    }
    char normalized[SENSOR_NAME_MAX];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return SENSOR_ERR_NAME_INVALID;
    }

    sensor_lock();
    if (!s_initialized) {
        memset(s_sensors, 0, sizeof(s_sensors));
        s_sensor_count = 0;
        s_initialized = true;
    }
    if (find_sensor_locked(normalized) >= 0) {
        sensor_unlock();
        return SENSOR_ERR_NAME_CONFLICT;
    }
    if (s_sensor_count >= SENSOR_MAX) {
        sensor_unlock();
        return SENSOR_ERR_NO_SPACE;
    }
    char reason[64];
    sensor_status_t status =
        validate_pin_for_kind_locked(pin, kind, reason, sizeof(reason));
    if (status != SENSOR_OK) {
        sensor_unlock();
        return status;
    }
    uint8_t channel = 0;
    if (kind == SENSOR_KIND_ADC1) {
        channel = adc1_channel_for_pin(pin);
        if (!adc_configure_channel(channel)) {
            sensor_unlock();
            return SENSOR_ERR_HARDWARE;
        }
    }

    sensor_record_t *r = &s_sensors[s_sensor_count++];
    memset(r, 0, sizeof(*r));
    copy_text(r->name, sizeof(r->name), normalized);
    r->kind = kind;
    r->pin = pin;
    r->adc_channel = channel;
    sample_record_locked(r);
    sensor_unlock();
    return SENSOR_OK;
}

sensor_status_t sensor_manager_remove(const char *name)
{
    char normalized[SENSOR_NAME_MAX];
    if (!normalize_name(name, normalized, sizeof(normalized))) {
        return SENSOR_ERR_INVALID_ARGUMENT;
    }
    sensor_lock();
    int index = find_sensor_locked(normalized);
    if (index < 0) {
        sensor_unlock();
        return SENSOR_ERR_NOT_FOUND;
    }
    for (size_t i = (size_t)index; i + 1 < s_sensor_count; ++i) {
        s_sensors[i] = s_sensors[i + 1];
    }
    s_sensor_count--;
    memset(&s_sensors[s_sensor_count], 0, sizeof(s_sensors[0]));
    sensor_unlock();
    return SENSOR_OK;
}

sensor_status_t sensor_manager_get(const char *name, sensor_sample_t *out)
{
    if (out == NULL) {
        return SENSOR_ERR_INVALID_ARGUMENT;
    }
    sensor_lock();
    int index = find_sensor_locked(name);
    if (index < 0) {
        sensor_unlock();
        return SENSOR_ERR_NOT_FOUND;
    }
    fill_status_locked(&s_sensors[index], out);
    sensor_unlock();
    return SENSOR_OK;
}

size_t sensor_manager_list(sensor_sample_t *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    sensor_lock();
    size_t count = s_sensor_count < capacity ? s_sensor_count : capacity;
    for (size_t i = 0; i < count; ++i) {
        fill_status_locked(&s_sensors[i], &out[i]);
    }
    sensor_unlock();
    return count;
}

sensor_status_t sensor_manager_sample(const char *name, sensor_sample_t *out)
{
    sensor_lock();
    int index = find_sensor_locked(name);
    if (index < 0) {
        sensor_unlock();
        return SENSOR_ERR_NOT_FOUND;
    }
    sample_record_locked(&s_sensors[index]);
    sensor_sample_t local;
    fill_status_locked(&s_sensors[index], &local);
    sensor_unlock();
    if (out != NULL) {
        *out = local;
    }
    return SENSOR_OK;
}

size_t sensor_manager_sample_all(void)
{
    sensor_lock();
    size_t valid = 0;
    for (size_t i = 0; i < s_sensor_count; ++i) {
        sample_record_locked(&s_sensors[i]);
        if (s_sensors[i].valid) {
            ++valid;
        }
    }
    sensor_unlock();
    return valid;
}

int32_t sensor_manager_value(const char *name, bool *valid)
{
    if (valid != NULL) {
        *valid = false;
    }
    sensor_sample_t status;
    if (sensor_manager_sample(name, &status) != SENSOR_OK) {
        return 0;
    }
    if (valid != NULL) {
        *valid = status.valid;
    }
    return status.value;
}

const char *sensor_kind_name(sensor_kind_t kind)
{
    switch (kind) {
    case SENSOR_KIND_DIGITAL: return "digital";
    case SENSOR_KIND_ADC1: return "adc1";
    default: return "unknown";
    }
}

const char *sensor_status_name(sensor_status_t status)
{
    switch (status) {
    case SENSOR_OK: return "ok";
    case SENSOR_ERR_INVALID_ARGUMENT: return "invalid-argument";
    case SENSOR_ERR_NO_SPACE: return "no-space";
    case SENSOR_ERR_NOT_FOUND: return "not-found";
    case SENSOR_ERR_NAME_CONFLICT: return "name-conflict";
    case SENSOR_ERR_NAME_INVALID: return "invalid-name";
    case SENSOR_ERR_PIN_NOT_READY: return "pin-not-ready";
    case SENSOR_ERR_NOT_INPUT: return "not-input";
    case SENSOR_ERR_NOT_ADC1: return "not-adc1";
    case SENSOR_ERR_ADC_USES_PULL: return "adc-uses-pull";
    case SENSOR_ERR_HARDWARE: return "hardware-error";
    case SENSOR_ERR_BAD_CONFIG: return "bad-config";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------------- */
/* Versioned configuration                                                   */
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
    uint8_t b[2] = {(uint8_t)(v & 0xff), (uint8_t)(v >> 8)};
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

size_t sensor_manager_export_config(uint8_t *buffer, size_t capacity,
                                    sensor_status_t *status)
{
    writer_t w = {
        .buffer = buffer,
        .capacity = capacity,
        .length = 0,
        .discard = buffer == NULL || capacity == 0,
        .overflow = false,
    };
    sensor_lock();
    w_u16(&w, SENSOR_CONFIG_SCHEMA_VERSION);
    w_u8(&w, (uint8_t)s_sensor_count);
    for (size_t i = 0; i < s_sensor_count; ++i) {
        const sensor_record_t *r = &s_sensors[i];
        uint8_t len = (uint8_t)strlen(r->name);
        w_u8(&w, len);
        w_bytes(&w, r->name, len);
        w_u8(&w, (uint8_t)r->kind);
        w_u8(&w, r->pin);
    }
    sensor_unlock();
    if (status != NULL) {
        *status = w.overflow ? SENSOR_ERR_INVALID_ARGUMENT : SENSOR_OK;
    }
    return w.length;
}

sensor_status_t sensor_manager_import_config(const uint8_t *buffer, size_t length)
{
    if (buffer == NULL || length < 3) {
        return SENSOR_ERR_BAD_CONFIG;
    }
    reader_t r = {.buffer = buffer, .length = length, .position = 0, .bad = false};
    if (r_u16(&r) != SENSOR_CONFIG_SCHEMA_VERSION) {
        return SENSOR_ERR_BAD_CONFIG;
    }
    uint8_t count = r_u8(&r);
    if (r.bad || count > SENSOR_MAX) {
        return SENSOR_ERR_BAD_CONFIG;
    }

    sensor_record_t parsed[SENSOR_MAX];
    memset(parsed, 0, sizeof(parsed));
    bool need_adc = false;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t name_len = r_u8(&r);
        if (name_len == 0 || name_len >= SENSOR_NAME_MAX) {
            return SENSOR_ERR_BAD_CONFIG;
        }
        const uint8_t *name = r_bytes(&r, name_len);
        uint8_t kind = r_u8(&r);
        uint8_t pin = r_u8(&r);
        if (name == NULL || r.bad || kind > SENSOR_KIND_ADC1 || pin >= PM_MAX_PINS) {
            return SENSOR_ERR_BAD_CONFIG;
        }
        char raw[PM_NAME_MAX];
        if (name_len >= sizeof(raw)) {
            return SENSOR_ERR_BAD_CONFIG;
        }
        memcpy(raw, name, name_len);
        raw[name_len] = '\0';
        if (!normalize_name(raw, parsed[i].name, sizeof(parsed[i].name))) {
            return SENSOR_ERR_BAD_CONFIG;
        }
        for (uint8_t j = 0; j < i; ++j) {
            if (strcmp(parsed[i].name, parsed[j].name) == 0) {
                return SENSOR_ERR_BAD_CONFIG;
            }
        }
        parsed[i].kind = (sensor_kind_t)kind;
        parsed[i].pin = pin;
        parsed[i].adc_channel = kind == SENSOR_KIND_ADC1 ? adc1_channel_for_pin(pin) : 0;
        char reason[64];
        if (validate_pin_for_kind_locked(pin, parsed[i].kind, reason,
                                         sizeof(reason)) != SENSOR_OK) {
            return SENSOR_ERR_BAD_CONFIG;
        }
        if (kind == SENSOR_KIND_ADC1) {
            need_adc = true;
            if (!adc_configure_channel(parsed[i].adc_channel)) {
                return SENSOR_ERR_HARDWARE;
            }
        }
    }
    if (r.bad || r.position != length) {
        return SENSOR_ERR_BAD_CONFIG;
    }

    sensor_lock();
    memcpy(s_sensors, parsed, sizeof(parsed));
    s_sensor_count = count;
    s_initialized = true;
    if (!need_adc) {
        adc_release_unit();
    }
    for (size_t i = 0; i < s_sensor_count; ++i) {
        sample_record_locked(&s_sensors[i]);
    }
    sensor_unlock();
    SENSOR_LOGI("restored %u sensor(s)", (unsigned)count);
    return SENSOR_OK;
}
