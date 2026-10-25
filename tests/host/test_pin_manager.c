#include "test_framework.h"

#include "GPIO_Sim.h"
#include "PinManager.h"
#include "device_registry.h"

static uint8_t find_pin(const pm_pin_status_t *pins, size_t n, const char *name)
{
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(pins[i].device_name, name) == 0) {
            return pins[i].pin;
        }
    }
    return 0xff;
}

void test_pin_manager(void)
{
    GPIO_TestResetAll();
    TEST_EQ_INT(PM_OK, pin_manager_init());
    pin_manager_test_set_time(1000);
    TEST_EQ_INT(PM_OK, pin_manager_reset());

    TEST_CASE("pin manager rejects unsafe initialization and unknown pins");
    TEST_EQ_INT(PM_ERR_RESERVED, pin_manager_pin_init(6, GPIO_MODE_OUTPUT, GPIO_PULL_NONE));
    TEST_EQ_INT(PM_ERR_RESERVED, pin_manager_pin_init(0, GPIO_MODE_INPUT, GPIO_PULL_NONE));
    TEST_EQ_INT(PM_ERR_INVALID_PIN,
                pin_manager_pin_init(40, GPIO_MODE_OUTPUT, GPIO_PULL_NONE));
    TEST_EQ_INT(PM_ERR_DIRECTION,
                pin_manager_pin_init(34, GPIO_MODE_OUTPUT, GPIO_PULL_NONE));
    TEST_EQ_INT(PM_ERR_UNSUPPORTED,
                pin_manager_pin_init(34, GPIO_MODE_INPUT, GPIO_PULL_UP));
    TEST_EQ_INT(PM_ERR_UNINITIALIZED,
                pin_manager_output(18, true, PM_ORIGIN_LOCAL, "test", 1.0f, "host"));
    TEST_EQ_INT(PM_ERR_UNINITIALIZED,
                pin_manager_assign(18, "fan1", true, false, false));

    TEST_CASE("default LED shape and AI authorization are explicit");
    TEST_EQ_INT(PM_OK, pin_manager_pin_init(23, GPIO_MODE_OUTPUT, GPIO_PULL_NONE));
    TEST_EQ_INT(PM_ERR_ALREADY_INITIALIZED,
                pin_manager_pin_init(23, GPIO_MODE_OUTPUT, GPIO_PULL_NONE));
    TEST_EQ_INT(PM_OK, pin_manager_assign(23, "LED1", true, true, true));
    TEST_EQ_INT(PM_OK, pin_manager_add_alias(23, "light"));
    TEST_EQ_INT(PM_OK, pin_manager_add_alias(23, "lamp"));
    TEST_EQ_INT(PM_ERR_ALREADY_CONFIGURED,
                pin_manager_assign(23, "other", true, false, false));
    TEST_EQ_INT(PM_ERR_NAME_CONFLICT, pin_manager_add_alias(23, "led1"));

    pm_pin_status_t status;
    TEST_EQ_INT(PM_OK, pin_manager_get_status(23, &status));
    TEST_EQ_STR("led1", status.device_name);
    TEST_EQ_INT(2, status.alias_count);
    TEST_ASSERT(status.configured);
    TEST_ASSERT(status.ai_allowed);
    TEST_ASSERT(status.test_approved);
    TEST_ASSERT(status.active_high);
    TEST_ASSERT(!status.logical_state);

    uint8_t found = 0;
    TEST_ASSERT(pin_manager_resolve_name("LIGHT", &found));
    TEST_EQ_INT(23, found);
    TEST_ASSERT(!pin_manager_resolve_name("missing", &found));

    TEST_EQ_INT(PM_OK,
                pin_manager_output(23, true, PM_ORIGIN_AI, "TURN_ON", 0.98f, "ai"));
    TEST_EQ_INT(PM_OK, pin_manager_get_status(23, &status));
    TEST_ASSERT(status.logical_state);
    TEST_ASSERT(status.physical_high);
    TEST_EQ_INT(PM_OK,
                pin_manager_toggle(23, PM_ORIGIN_AI, "TOGGLE", 0.95f, "ai"));
    TEST_EQ_INT(PM_OK, pin_manager_get_status(23, &status));
    TEST_ASSERT(!status.logical_state);

    TEST_CASE("pin manager enforces AI and local authorization separately");
    TEST_EQ_INT(PM_OK, pin_manager_pin_init(18, GPIO_MODE_OUTPUT, GPIO_PULL_NONE));
    TEST_EQ_INT(PM_OK, pin_manager_assign(18, "fan1", true, false, false));
    TEST_EQ_INT(PM_ERR_AI_DENIED,
                pin_manager_output(18, true, PM_ORIGIN_AI, "TURN_ON", 1.0f, "ai"));
    TEST_EQ_INT(PM_OK,
                pin_manager_output(18, true, PM_ORIGIN_LOCAL, "on", 1.0f, "console"));
    TEST_EQ_INT(PM_OK, pin_manager_set_ai_allowed(18, true));
    TEST_EQ_INT(PM_OK,
                pin_manager_output(18, false, PM_ORIGIN_AI, "TURN_OFF", 1.0f, "ai"));

    TEST_CASE("locking a pin blocks all control until explicitly unlocked");
    TEST_EQ_INT(PM_OK, pin_manager_set_locked(23, true));
    TEST_EQ_INT(PM_ERR_LOCKED,
                pin_manager_output(23, true, PM_ORIGIN_LOCAL, "on", 1.0f, "console"));
    TEST_EQ_INT(PM_ERR_LOCKED,
                pin_manager_toggle(23, PM_ORIGIN_AI, "TOGGLE", 1.0f, "ai"));
    TEST_EQ_INT(PM_ERR_LOCKED, pin_manager_pin_deinit(23));
    TEST_EQ_INT(PM_OK, pin_manager_set_locked(23, false));

    TEST_CASE("active-low assignment latches off safely and drives logical modes");
    TEST_EQ_INT(PM_OK, pin_manager_pin_init(21, GPIO_MODE_OUTPUT, GPIO_PULL_NONE));
    TEST_EQ_INT(PM_OK, pin_manager_assign(21, "relay1", false, true, false));
    TEST_EQ_INT(PM_OK, pin_manager_get_status(21, &status));
    TEST_ASSERT(!status.active_high);
    TEST_ASSERT(!status.logical_state);
    TEST_ASSERT(status.physical_high);
    TEST_ASSERT(GPIO_SimGetOutputLatch(21));
    TEST_ASSERT(GPIO_SimGetOutputEnable(21));
    TEST_EQ_INT(1, g_gpio_sim.latch_at_enable[21]);
    TEST_EQ_INT(PM_OK,
                pin_manager_output(21, true, PM_ORIGIN_AI, "TURN_ON", 1.0f, "ai"));
    TEST_EQ_INT(PM_OK, pin_manager_get_status(21, &status));
    TEST_ASSERT(status.logical_state);
    TEST_ASSERT(!status.physical_high);
    TEST_EQ_INT(PM_OK,
                pin_manager_output(21, false, PM_ORIGIN_AI, "TURN_OFF", 1.0f, "ai"));

    TEST_CASE("groups and bulk operations require confirmation for multiple AI targets");
    TEST_EQ_INT(PM_OK, pin_manager_create_group("lights"));
    TEST_EQ_INT(PM_OK, pin_manager_add_group_pin("lights", 23));
    TEST_EQ_INT(PM_ERR_NAME_CONFLICT, pin_manager_create_group("lights"));
    TEST_EQ_INT(PM_ERR_NOT_FOUND, pin_manager_group_apply(
                                      "missing", true, false, PM_ORIGIN_AI, "on", 1.0f,
                                      "ai", NULL));
    TEST_EQ_INT(PM_OK, pin_manager_group_apply("lights", true, false, PM_ORIGIN_AI,
                                               "TURN_ON", 1.0f, "ai", NULL));
    TEST_EQ_INT(PM_OK, pin_manager_get_status(23, &status));
    TEST_ASSERT(status.logical_state);

    TEST_EQ_INT(PM_OK, pin_manager_create_group("testtwo"));
    TEST_EQ_INT(PM_OK, pin_manager_add_group_pin("testtwo", 18));
    TEST_EQ_INT(PM_OK, pin_manager_add_group_pin("testtwo", 21));
    size_t affected = 0;
    TEST_EQ_INT(PM_ERR_CONFIRMATION,
                pin_manager_group_apply("testtwo", true, false, PM_ORIGIN_AI,
                                        "TURN_ON", 1.0f, "ai", &affected));
    TEST_EQ_INT(0, affected);
    TEST_EQ_INT(PM_OK, pin_manager_get_status(18, &status));
    TEST_ASSERT(!status.logical_state);
    TEST_EQ_INT(PM_OK, pin_manager_group_apply("testtwo", true, true, PM_ORIGIN_AI,
                                               "TURN_ON", 1.0f, "ai", &affected));
    TEST_EQ_INT(2, affected);

    TEST_CASE("bulk ON is restricted to approved outputs and bulk OFF is safe");
    TEST_EQ_INT(PM_OK, pin_manager_output(18, true, PM_ORIGIN_LOCAL, "on", 1.0f,
                                          "console"));
    TEST_EQ_INT(PM_OK, pin_manager_outputs_all_off(PM_ORIGIN_LOCAL, "console", NULL));
    TEST_EQ_INT(PM_OK, pin_manager_get_status(18, &status));
    TEST_ASSERT(!status.logical_state);
    TEST_EQ_INT(PM_ERR_CONFIRMATION, pin_manager_outputs_all_on(false, "console", NULL));
    TEST_EQ_INT(PM_OK, pin_manager_outputs_all_on(true, "console", &affected));
    TEST_EQ_INT(1, affected);
    TEST_EQ_INT(PM_OK, pin_manager_get_status(23, &status));
    TEST_ASSERT(status.logical_state);
    TEST_EQ_INT(PM_OK, pin_manager_get_status(18, &status));
    TEST_ASSERT(!status.logical_state);
    TEST_EQ_INT(PM_ERR_UNSUPPORTED,
                pin_manager_outputs_all_off(PM_ORIGIN_AI, "ai", NULL));

    TEST_CASE("configuration export/import validates and never restores ON");
    TEST_EQ_INT(PM_OK, pin_manager_output(23, true, PM_ORIGIN_AI, "TURN_ON", 1.0f,
                                          "ai"));
    uint8_t blob[2048];
    pm_status_t export_status = PM_OK;
    size_t blob_size = pin_manager_export_config(blob, sizeof(blob), &export_status);
    TEST_EQ_INT(PM_OK, export_status);
    TEST_ASSERT(blob_size > 4);
    TEST_ASSERT(blob_size < sizeof(blob));

    TEST_EQ_INT(PM_OK, pin_manager_import_config(blob, blob_size));
    TEST_EQ_INT(PM_OK, pin_manager_get_status(23, &status));
    TEST_ASSERT(status.initialized && status.configured && status.ai_allowed);
    TEST_ASSERT(status.active_high);
    TEST_ASSERT(!status.logical_state);
    TEST_ASSERT(!status.physical_high);
    TEST_EQ_INT(PM_OK, pin_manager_get_status(21, &status));
    TEST_ASSERT(!status.active_high);
    TEST_ASSERT(!status.logical_state);
    TEST_ASSERT(status.physical_high);

    uint8_t bad[2048];
    memcpy(bad, blob, blob_size);
    bad[blob_size - 1] ^= 0xff;
    TEST_EQ_INT(PM_ERR_BAD_CONFIG, pin_manager_import_config(bad, blob_size));
    TEST_EQ_INT(PM_ERR_BAD_CONFIG, pin_manager_import_config(blob, 3));
    TEST_EQ_INT(PM_ERR_BAD_CONFIG, pin_manager_import_config(NULL, 10));

    pm_group_status_t groups[PM_MAX_GROUPS];
    size_t group_count = pin_manager_list_groups(groups, PM_MAX_GROUPS);
    TEST_EQ_INT(2, group_count);
    pm_pin_status_t pins[PM_MAX_PINS];
    size_t pin_count = pin_manager_list(pins, PM_MAX_PINS, true);
    TEST_EQ_INT(3, pin_count);
    TEST_EQ_INT(23, find_pin(pins, pin_count, "led1"));

    device_info_t devices[8];
    size_t device_count = device_registry_list(devices, 8, false);
    TEST_EQ_INT(3, device_count);
    TEST_ASSERT(device_registry_output_exists(23));
    TEST_ASSERT(device_registry_resolve("light", &found));
    TEST_EQ_INT(23, found);

    TEST_CASE("action log records time, origin, target and outcome");
    pm_action_log_entry_t log[PM_LOG_CAPACITY];
    size_t log_count = pin_manager_get_log(log, PM_LOG_CAPACITY);
    TEST_ASSERT(log_count > 0);
    TEST_ASSERT(log[log_count - 1].seq > 0);
    TEST_ASSERT(log[log_count - 1].timestamp_ms >= 1000);
    TEST_ASSERT(log[log_count - 1].origin <= PM_ORIGIN_REMOTE);
    pin_manager_clear_log();
    TEST_EQ_INT(0, pin_manager_get_log(log, PM_LOG_CAPACITY));

    TEST_CASE("release and reset clear ownership and drive outputs off");
    TEST_EQ_INT(PM_OK, pin_manager_pin_init(27, GPIO_MODE_INPUT, GPIO_PULL_UP));
    TEST_EQ_INT(PM_OK, pin_manager_assign(27, "button1", true, false, false));
    TEST_EQ_INT(PM_OK, pin_manager_release(27));
    TEST_EQ_INT(PM_ERR_NOT_CONFIGURED, pin_manager_release(27));
    TEST_EQ_INT(PM_OK, pin_manager_output(23, true, PM_ORIGIN_LOCAL, "on", 1.0f,
                                          "console"));
    TEST_EQ_INT(PM_OK, pin_manager_reset());
    TEST_EQ_INT(PM_OK, pin_manager_get_status(23, &status));
    TEST_ASSERT(!status.initialized);
    TEST_ASSERT(!GPIO_SimGetOutputEnable(23));
}
