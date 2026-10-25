#include "config_store.h"

#include <stdio.h>
#include <string.h>

#include "Automation.h"
#include "PinManager.h"
#include "SensorManager.h"

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

#define CFG_NAMESPACE "tinyai_cfg"
#define CFG_KEY_PINS "pins"
#define CFG_KEY_SENSORS "sensors"
#define CFG_KEY_RULES "rules"
#define CFG_MAGIC0 'T'
#define CFG_MAGIC1 'A'
#define CFG_MAGIC2 'C'
#define CFG_MAGIC3 'F'
#define CFG_ENVELOPE_VERSION 1u
#define CFG_HEADER_BYTES 16u
#define CFG_MAX_PAYLOAD 4096u

static char s_detail[160] = "not loaded";

static void set_detail(const char *text)
{
    if (text == NULL) {
        s_detail[0] = '\0';
        return;
    }
    snprintf(s_detail, sizeof(s_detail), "%s", text);
}

static void set_detail_section(const char *section, const char *text)
{
    snprintf(s_detail, sizeof(s_detail), "%s: %s", section, text);
}

#ifdef ESP_PLATFORM

static uint8_t s_blob[CFG_HEADER_BYTES + CFG_MAX_PAYLOAD];
static uint8_t s_payload[CFG_MAX_PAYLOAD];

static uint32_t crc32_bytes(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static config_store_status_t save_section(nvs_handle_t handle, const char *key,
                                          uint16_t schema,
                                          const uint8_t *payload,
                                          size_t payload_length,
                                          const char *section)
{
    if (payload_length > CFG_MAX_PAYLOAD) {
        set_detail_section(section, "configuration is too large");
        return CFG_NO_SPACE;
    }

    s_blob[0] = CFG_MAGIC0;
    s_blob[1] = CFG_MAGIC1;
    s_blob[2] = CFG_MAGIC2;
    s_blob[3] = CFG_MAGIC3;
    put_u16(s_blob + 4, CFG_ENVELOPE_VERSION);
    put_u16(s_blob + 6, schema);
    put_u32(s_blob + 8, (uint32_t)payload_length);
    put_u32(s_blob + 12, crc32_bytes(payload, payload_length));
    memcpy(s_blob + CFG_HEADER_BYTES, payload, payload_length);

    esp_err_t err = nvs_set_blob(handle, key, s_blob,
                                 CFG_HEADER_BYTES + payload_length);
    if (err != ESP_OK) {
        set_detail_section(section, "NVS write failed");
        return CFG_IO_ERROR;
    }
    return CFG_OK;
}

static config_store_status_t load_section(nvs_handle_t handle, const char *key,
                                          uint16_t expected_schema,
                                          const char *section, size_t *out_length)
{
    size_t length = 0;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_detail_section(section, "not found");
        return CFG_NOT_FOUND;
    }
    if (err != ESP_OK) {
        set_detail_section(section, "NVS size query failed");
        return CFG_IO_ERROR;
    }
    if (length < CFG_HEADER_BYTES) {
        set_detail_section(section, "saved data is truncated");
        return CFG_INVALID;
    }
    if (length > sizeof(s_blob) ||
        length - CFG_HEADER_BYTES > CFG_MAX_PAYLOAD) {
        set_detail_section(section, "saved data is too large");
        return CFG_NO_SPACE;
    }
    err = nvs_get_blob(handle, key, s_blob, &length);
    if (err != ESP_OK) {
        set_detail_section(section, "NVS read failed");
        return CFG_IO_ERROR;
    }
    if (s_blob[0] != CFG_MAGIC0 || s_blob[1] != CFG_MAGIC1 ||
        s_blob[2] != CFG_MAGIC2 || s_blob[3] != CFG_MAGIC3) {
        set_detail_section(section, "bad magic");
        return CFG_INVALID;
    }
    if (get_u16(s_blob + 4) != CFG_ENVELOPE_VERSION) {
        set_detail_section(section, "unsupported envelope version");
        return CFG_UNSUPPORTED;
    }
    uint16_t schema = get_u16(s_blob + 6);
    if (schema != expected_schema) {
        set_detail_section(section, "unsupported schema version");
        return CFG_UNSUPPORTED;
    }
    uint32_t payload_length = get_u32(s_blob + 8);
    if (payload_length != length - CFG_HEADER_BYTES ||
        payload_length > CFG_MAX_PAYLOAD) {
        set_detail_section(section, "bad payload length");
        return CFG_INVALID;
    }
    if (get_u32(s_blob + 12) !=
        crc32_bytes(s_blob + CFG_HEADER_BYTES, payload_length)) {
        set_detail_section(section, "CRC mismatch");
        return CFG_INVALID;
    }
    memcpy(s_payload, s_blob + CFG_HEADER_BYTES, payload_length);
    *out_length = payload_length;
    return CFG_OK;
}

config_store_status_t config_store_save(void)
{
    pm_status_t export_status = PM_OK;
    size_t pins_length =
        pin_manager_export_config(s_payload, CFG_MAX_PAYLOAD, &export_status);
    if (export_status != PM_OK || pins_length < 2) {
        set_detail("pins: export failed");
        return export_status == PM_ERR_NO_SPACE ? CFG_NO_SPACE : CFG_IO_ERROR;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        set_detail("NVS open failed");
        return CFG_IO_ERROR;
    }

    config_store_status_t status =
        save_section(handle, CFG_KEY_PINS, PM_CONFIG_SCHEMA_VERSION, s_payload,
                     pins_length, "pins");
    if (status != CFG_OK) {
        nvs_close(handle);
        return status;
    }

    sensor_status_t sensor_status = SENSOR_OK;
    size_t sensors_length = sensor_manager_export_config(
        s_payload, CFG_MAX_PAYLOAD, &sensor_status);
    if (sensor_status != SENSOR_OK || sensors_length < 3) {
        set_detail("sensors: export failed");
        nvs_close(handle);
        return CFG_IO_ERROR;
    }
    status = save_section(handle, CFG_KEY_SENSORS, SENSOR_CONFIG_SCHEMA_VERSION,
                          s_payload, sensors_length, "sensors");
    if (status != CFG_OK) {
        nvs_close(handle);
        return status;
    }

    automation_config_status_t rule_status = AUTOMATION_CONFIG_OK;
    size_t rules_length = automation_export_config(s_payload, CFG_MAX_PAYLOAD,
                                                   &rule_status);
    if (rule_status != AUTOMATION_CONFIG_OK || rules_length < 3) {
        set_detail("rules: export failed");
        nvs_close(handle);
        return rule_status == AUTOMATION_CONFIG_NO_SPACE ? CFG_NO_SPACE
                                                         : CFG_IO_ERROR;
    }
    status = save_section(handle, CFG_KEY_RULES, AUTOMATION_CONFIG_SCHEMA_VERSION,
                          s_payload, rules_length, "rules");
    if (status != CFG_OK) {
        nvs_close(handle);
        return status;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        set_detail("NVS commit failed");
        return CFG_IO_ERROR;
    }
    set_detail("pins, sensors and rules saved");
    return CFG_OK;
}

config_store_status_t config_store_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_detail("no saved configuration");
        return CFG_NOT_FOUND;
    }
    if (err != ESP_OK) {
        set_detail("NVS open failed");
        return CFG_IO_ERROR;
    }

    size_t pins_length = 0;
    config_store_status_t status =
        load_section(handle, CFG_KEY_PINS, PM_CONFIG_SCHEMA_VERSION, "pins",
                     &pins_length);
    if (status != CFG_OK) {
        nvs_close(handle);
        return status;
    }
    pm_status_t pm = pin_manager_import_config(s_payload, pins_length);
    if (pm != PM_OK) {
        set_detail_section("pins", "rejected");
        nvs_close(handle);
        return CFG_INVALID;
    }

    /* The saved pin set is the foundation. Sensors and rules are rebuilt from
     * their own sections; stale in-memory entries must not survive a load. */
    (void)sensor_manager_reset();
    automation_clear();

    size_t sensors_length = 0;
    status = load_section(handle, CFG_KEY_SENSORS, SENSOR_CONFIG_SCHEMA_VERSION,
                          "sensors", &sensors_length);
    if (status != CFG_OK) {
        set_detail_section("sensors", "pins loaded; sensor section rejected");
        nvs_close(handle);
        return CFG_PARTIAL;
    }
    sensor_status_t sensor_status =
        sensor_manager_import_config(s_payload, sensors_length);
    if (sensor_status != SENSOR_OK) {
        (void)sensor_manager_reset();
        set_detail_section("sensors", "pins loaded; sensor section rejected");
        nvs_close(handle);
        return CFG_PARTIAL;
    }

    size_t rules_length = 0;
    status = load_section(handle, CFG_KEY_RULES, AUTOMATION_CONFIG_SCHEMA_VERSION,
                          "rules", &rules_length);
    if (status != CFG_OK) {
        automation_clear();
        set_detail_section("rules", "pins and sensors loaded; rule section rejected");
        nvs_close(handle);
        return CFG_PARTIAL;
    }
    automation_config_status_t rule_status =
        automation_import_config(s_payload, rules_length);
    if (rule_status != AUTOMATION_CONFIG_OK) {
        automation_clear();
        set_detail_section("rules", "pins and sensors loaded; rules rejected");
        nvs_close(handle);
        return CFG_PARTIAL;
    }

    nvs_close(handle);
    set_detail("pins, sensors and rules loaded; outputs are OFF");
    return CFG_OK;
}

config_store_status_t config_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        set_detail("NVS open failed");
        return CFG_IO_ERROR;
    }
    const char *keys[] = {CFG_KEY_PINS, CFG_KEY_SENSORS, CFG_KEY_RULES};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        err = nvs_erase_key(handle, keys[i]);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            set_detail("NVS erase failed");
            return CFG_IO_ERROR;
        }
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        set_detail("NVS commit failed");
        return CFG_IO_ERROR;
    }
    set_detail("saved pins, sensors and rules erased");
    return CFG_OK;
}

config_store_status_t config_store_probe(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CFG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? CFG_NOT_FOUND : CFG_IO_ERROR;
    }

    const char *keys[] = {CFG_KEY_PINS, CFG_KEY_SENSORS, CFG_KEY_RULES};
    const char *names[] = {"pins", "sensors", "rules"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        size_t length = 0;
        err = nvs_get_blob(handle, keys[i], NULL, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            snprintf(s_detail, sizeof(s_detail), "%s: not found", names[i]);
            return i == 0 ? CFG_NOT_FOUND : CFG_INVALID;
        }
        if (err != ESP_OK) {
            nvs_close(handle);
            set_detail("NVS probe failed");
            return CFG_IO_ERROR;
        }
        if (length < CFG_HEADER_BYTES) {
            nvs_close(handle);
            snprintf(s_detail, sizeof(s_detail), "%s: saved data is truncated",
                     names[i]);
            return CFG_INVALID;
        }
    }
    nvs_close(handle);
    set_detail("pins, sensors and rules present");
    return CFG_OK;
}

#else /* host and other non-ESP builds */

config_store_status_t config_store_save(void)
{
    set_detail("NVS unavailable in host build");
    return CFG_UNSUPPORTED;
}

config_store_status_t config_store_load(void)
{
    set_detail("NVS unavailable in host build");
    return CFG_UNSUPPORTED;
}

config_store_status_t config_store_erase(void)
{
    set_detail("NVS unavailable in host build");
    return CFG_UNSUPPORTED;
}

config_store_status_t config_store_probe(void)
{
    set_detail("NVS unavailable in host build");
    return CFG_UNSUPPORTED;
}

#endif

const char *config_store_status_name(config_store_status_t status)
{
    switch (status) {
    case CFG_OK: return "ok";
    case CFG_PARTIAL: return "partial";
    case CFG_NOT_FOUND: return "not-found";
    case CFG_INVALID: return "invalid";
    case CFG_UNSUPPORTED: return "unsupported";
    case CFG_IO_ERROR: return "io-error";
    case CFG_NO_SPACE: return "no-space";
    default: return "unknown";
    }
}

const char *config_store_last_detail(void)
{
    return s_detail;
}
