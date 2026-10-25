#include <stdio.h>

#include "Automation.h"
#include "CommandEngine.h"
#include "PinManager.h"
#include "SensorManager.h"
#include "SerialConsole.h"
#include "board_profile.h"
#include "config_store.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "app";

#ifdef CONFIG_TINYAI_AI_DEFAULT_ENABLED
#define TINYAI_AI_DEFAULT_ENABLED CONFIG_TINYAI_AI_DEFAULT_ENABLED
#else
#define TINYAI_AI_DEFAULT_ENABLED 0
#endif

static void automation_task(void *argument)
{
    (void)argument;
    for (;;) {
        (void)automation_update((uint32_t)(esp_timer_get_time() / 1000));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs reinitialization; erasing NVS partition");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

/* The only automatic device creation in the firmware. It is deliberately a
 * harmless test output, never a relay or motor. Everything else is created by
 * the local console and persisted only by an explicit "save". */
static pm_status_t apply_default_led1(void)
{
    pm_status_t status = pin_manager_pin_init(23, GPIO_MODE_OUTPUT, GPIO_PULL_NONE);
    if (status != PM_OK) {
        return status;
    }
    status = pin_manager_assign(23, "led1", true, true, true);
    if (status != PM_OK) {
        (void)pin_manager_pin_deinit(23);
        return status;
    }
    status = pin_manager_add_alias(23, "light");
    if (status != PM_OK) {
        return status;
    }
    status = pin_manager_add_alias(23, "lamp");
    if (status != PM_OK) {
        return status;
    }
    status = pin_manager_add_alias(23, "ledone");
    if (status != PM_OK) {
        return status;
    }
    status = pin_manager_add_alias(23, "led one");
    if (status != PM_OK) {
        return status;
    }
    status = pin_manager_create_group("lights");
    if (status != PM_OK && status != PM_ERR_NAME_CONFLICT) {
        return status;
    }
    status = pin_manager_add_group_pin("lights", 23);
    if (status != PM_OK && status != PM_ERR_ALREADY_CONFIGURED) {
        return status;
    }
    return PM_OK;
}

void app_main(void)
{
    board_profile_init();
    ESP_LOGI(TAG, "heap before init: free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    esp_err_t nvs = init_nvs();
    if (nvs != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s; saved configuration is unavailable",
                 esp_err_to_name(nvs));
    }

    pm_status_t pm = pin_manager_init();
    if (pm != PM_OK) {
        ESP_LOGE(TAG, "pin manager init failed: %s",
                 pin_manager_status_name(pm));
        return;
    }

    config_store_status_t config = CFG_IO_ERROR;
    bool using_default = false;
    if (nvs == ESP_OK) {
        config = config_store_load();
    }
    if (config == CFG_OK || config == CFG_PARTIAL) {
        size_t affected = 0;
        pm_status_t off = pin_manager_outputs_all_off(
            PM_ORIGIN_LOCAL, "boot-safety", &affected);
        if (off != PM_OK) {
            ESP_LOGW(TAG, "boot output shutdown: %s",
                     pin_manager_status_name(off));
        }
        if (config == CFG_OK) {
            ESP_LOGI(TAG, "configuration restored; all outputs forced OFF");
        } else {
            ESP_LOGW(TAG,
                     "pins restored; %s; outputs forced OFF and no sensors or "
                     "rules are active",
                     config_store_last_detail());
        }
    } else {
        if (config == CFG_NOT_FOUND) {
            ESP_LOGI(TAG, "no saved configuration; applying safe default led1");
        } else if (nvs == ESP_OK) {
            ESP_LOGW(TAG, "saved configuration rejected: %s (%s); using defaults",
                     config_store_status_name(config), config_store_last_detail());
        } else {
            ESP_LOGW(TAG, "NVS unavailable; using safe default led1");
        }
        pm_status_t reset = pin_manager_reset();
        if (reset != PM_OK) {
            ESP_LOGE(TAG, "pin manager reset failed: %s",
                     pin_manager_status_name(reset));
            return;
        }
        pm_status_t defaults = apply_default_led1();
        if (defaults != PM_OK) {
            ESP_LOGE(TAG, "default led1 setup failed: %s",
                     pin_manager_status_name(defaults));
            return;
        }
        using_default = true;
    }

    sensor_manager_init();
    automation_init();
    command_engine_init(TINYAI_AI_DEFAULT_ENABLED);

    BaseType_t automation_created =
        xTaskCreate(automation_task, "automation", 4096, NULL, 3, NULL);
    if (automation_created != pdPASS) {
        ESP_LOGE(TAG, "could not create automation task; automation is inactive");
    }

    if (serial_console_start() != 0) {
        ESP_LOGE(TAG, "serial console could not start; automation still runs");
    }

    ESP_LOGI(TAG, "ready: config=%s AI compiled=%s enabled=%s, heap free=%u",
             using_default ? "defaults" : config_store_status_name(config),
             command_engine_ai_available() ? "yes" : "no",
             command_engine_ai_enabled() ? "yes" : "no",
             (unsigned)esp_get_free_heap_size());
    ESP_LOGI(TAG, "heap after init: free=%u min=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
