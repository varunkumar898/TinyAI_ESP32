#include "test_framework.h"

#include "Automation.h"
#include "GPIO_Sim.h"
#include "PinManager.h"
#include "SensorManager.h"

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void configure_pin(uint8_t pin, const char *name, GPIO_Mode mode,
                          GPIO_Pull pull)
{
    TEST_EQ_INT(PM_OK, pin_manager_pin_init(pin, mode, pull));
    TEST_EQ_INT(PM_OK, pin_manager_assign(pin, name, true, true,
                                          mode == GPIO_MODE_OUTPUT));
}

static bool pin_on(uint8_t pin)
{
    pm_pin_status_t status;
    if (pin_manager_get_status(pin, &status) != PM_OK) {
        return false;
    }
    return status.logical_state;
}

void test_serialization(void)
{
    pin_manager_reset();
    GPIO_TestResetAll();
    TEST_EQ_INT(PM_OK, pin_manager_init());
    TEST_EQ_INT(PM_OK, pin_manager_reset());
    sensor_manager_init();
    TEST_EQ_INT(SENSOR_OK, sensor_manager_reset());
    automation_init();
    automation_clear();

    configure_pin(23, "led1", GPIO_MODE_OUTPUT, GPIO_PULL_NONE);
    configure_pin(25, "led2", GPIO_MODE_OUTPUT, GPIO_PULL_NONE);
    configure_pin(27, "motion", GPIO_MODE_INPUT, GPIO_PULL_UP);
    configure_pin(34, "lux", GPIO_MODE_INPUT, GPIO_PULL_NONE);

    TEST_CASE("sensor configuration export has a versioned deterministic layout");
    TEST_EQ_INT(SENSOR_OK, sensor_manager_add("lux", SENSOR_KIND_ADC1, 34));
    TEST_EQ_INT(SENSOR_OK, sensor_manager_add("motion", SENSOR_KIND_DIGITAL, 27));

    uint8_t sensors_blob[256];
    sensor_status_t sensor_export = SENSOR_ERR_INVALID_ARGUMENT;
    size_t sensors_length =
        sensor_manager_export_config(sensors_blob, sizeof(sensors_blob),
                                     &sensor_export);
    TEST_EQ_INT(SENSOR_OK, sensor_export);
    TEST_ASSERT(sensors_length > 8);
    TEST_ASSERT(sensors_length < sizeof(sensors_blob));
    TEST_EQ_INT(SENSOR_CONFIG_SCHEMA_VERSION, read_u16(sensors_blob));
    TEST_EQ_INT(2, sensors_blob[2]);
    TEST_EQ_INT(3, sensors_blob[3]);
    TEST_ASSERT(memcmp(&sensors_blob[4], "lux", 3) == 0);
    TEST_EQ_INT(SENSOR_KIND_ADC1, sensors_blob[7]);
    TEST_EQ_INT(34, sensors_blob[8]);

    uint8_t tiny[4];
    sensor_status_t tiny_status = SENSOR_OK;
    size_t tiny_length =
        sensor_manager_export_config(tiny, sizeof(tiny), &tiny_status);
    TEST_EQ_INT(SENSOR_ERR_INVALID_ARGUMENT, tiny_status);
    TEST_ASSERT(tiny_length > sizeof(tiny));

    TEST_CASE("sensor import validates before replacing the live registry");
    TEST_EQ_INT(SENSOR_OK, sensor_manager_reset());
    TEST_EQ_INT(0, sensor_manager_list(NULL, 0));
    TEST_EQ_INT(SENSOR_OK,
                sensor_manager_import_config(sensors_blob, sensors_length));
    sensor_sample_t samples[SENSOR_MAX];
    TEST_EQ_INT(2, sensor_manager_list(samples, SENSOR_MAX));
    TEST_EQ_STR("lux", samples[0].name);
    TEST_EQ_INT(34, samples[0].pin);
    TEST_EQ_STR("motion", samples[1].name);
    TEST_EQ_INT(27, samples[1].pin);

    uint8_t bad_sensors[256];
    memcpy(bad_sensors, sensors_blob, sensors_length);
    bad_sensors[0] = 9;
    TEST_EQ_INT(SENSOR_ERR_BAD_CONFIG,
                sensor_manager_import_config(bad_sensors, sensors_length));
    TEST_EQ_INT(2, sensor_manager_list(samples, SENSOR_MAX));

    memcpy(bad_sensors, sensors_blob, sensors_length);
    bad_sensors[sensors_length] = 0xaa;
    TEST_EQ_INT(SENSOR_ERR_BAD_CONFIG,
                sensor_manager_import_config(bad_sensors, sensors_length + 1));
    TEST_EQ_INT(2, sensor_manager_list(samples, SENSOR_MAX));

    TEST_EQ_INT(SENSOR_ERR_BAD_CONFIG,
                sensor_manager_import_config(sensors_blob, sensors_length - 1));
    TEST_EQ_INT(SENSOR_ERR_BAD_CONFIG,
                sensor_manager_import_config(NULL, sensors_length));
    TEST_EQ_INT(SENSOR_ERR_BAD_CONFIG, sensor_manager_import_config(sensors_blob, 2));
    TEST_EQ_INT(2, sensor_manager_list(samples, SENSOR_MAX));

    TEST_CASE("sensor import revalidates GPIO ownership and direction");
    TEST_EQ_INT(PM_OK, pin_manager_pin_deinit(27));
    TEST_EQ_INT(SENSOR_ERR_BAD_CONFIG,
                sensor_manager_import_config(sensors_blob, sensors_length));
    TEST_EQ_INT(2, sensor_manager_list(samples, SENSOR_MAX));
    configure_pin(27, "motion", GPIO_MODE_INPUT, GPIO_PULL_UP);
    TEST_EQ_INT(SENSOR_OK,
                sensor_manager_import_config(sensors_blob, sensors_length));

    TEST_CASE("rule export is versioned and round-trips configuration only");
    TEST_EQ_INT(AUTOMATION_RULE_OK,
                automation_rule_add("hot", "lux", 23, AUTOMATION_ABOVE, 600, 100));
    TEST_EQ_INT(AUTOMATION_RULE_OK,
                automation_rule_add("cool", "motion", 25, AUTOMATION_BELOW, 1, 0));

    uint8_t rules_blob[256];
    automation_config_status_t rule_export = AUTOMATION_CONFIG_BAD_CONFIG;
    size_t rules_length =
        automation_export_config(rules_blob, sizeof(rules_blob), &rule_export);
    TEST_EQ_INT(AUTOMATION_CONFIG_OK, rule_export);
    TEST_ASSERT(rules_length > 5);
    TEST_EQ_INT(AUTOMATION_CONFIG_SCHEMA_VERSION, read_u16(rules_blob));
    TEST_EQ_INT(2, rules_blob[2]);

    TEST_EQ_INT(AUTOMATION_RULE_OK, automation_rule_set_enabled("cool", false));
    uint8_t disabled_blob[256];
    automation_config_status_t disabled_export = AUTOMATION_CONFIG_BAD_CONFIG;
    size_t disabled_length = automation_export_config(
        disabled_blob, sizeof(disabled_blob), &disabled_export);
    TEST_EQ_INT(AUTOMATION_CONFIG_OK, disabled_export);
    TEST_ASSERT(disabled_length > 5);
    TEST_ASSERT(disabled_length != rules_length ||
                memcmp(disabled_blob, rules_blob, rules_length) != 0);

    automation_set_enabled(true);
    sensor_manager_test_set_adc_raw(34, 3000);
    TEST_EQ_INT(1, automation_update(1000));
    TEST_ASSERT(pin_on(23));

    automation_clear();
    TEST_EQ_INT(0, automation_rule_count());
    TEST_ASSERT(pin_on(23));

    TEST_EQ_INT(AUTOMATION_CONFIG_OK,
                automation_import_config(rules_blob, rules_length));
    TEST_EQ_INT(2, automation_rule_count());
    automation_rule_info_t rules[AUTOMATION_MAX_RULES];
    automation_rule_list(rules, AUTOMATION_MAX_RULES);
    TEST_EQ_STR("hot", rules[0].name);
    TEST_EQ_STR("lux", rules[0].sensor);
    TEST_EQ_INT(23, rules[0].output_pin);
    TEST_EQ_INT(AUTOMATION_ABOVE, rules[0].condition);
    TEST_EQ_INT(600, rules[0].threshold);
    TEST_EQ_INT(100, rules[0].hysteresis);
    TEST_ASSERT(rules[0].enabled);
    TEST_ASSERT(!rules[0].active);
    TEST_ASSERT(!rules[0].output_owned);
    TEST_EQ_STR("cool", rules[1].name);
    TEST_ASSERT(rules[1].enabled);
    TEST_ASSERT(pin_on(23));

    TEST_CASE("rule import rejects corruption and missing prerequisites");
    uint8_t bad_rules[256];
    memcpy(bad_rules, rules_blob, rules_length);
    bad_rules[rules_length - 1] ^= 0xff;
    TEST_EQ_INT(AUTOMATION_CONFIG_BAD_CONFIG,
                automation_import_config(bad_rules, rules_length));
    TEST_EQ_INT(2, automation_rule_count());

    TEST_EQ_INT(AUTOMATION_CONFIG_BAD_CONFIG,
                automation_import_config(rules_blob, rules_length - 1));
    TEST_EQ_INT(AUTOMATION_CONFIG_BAD_CONFIG, automation_import_config(rules_blob, 2));
    TEST_EQ_INT(AUTOMATION_CONFIG_BAD_CONFIG, automation_import_config(NULL, 2));
    TEST_EQ_INT(2, automation_rule_count());

    TEST_EQ_INT(SENSOR_OK, sensor_manager_remove("lux"));
    TEST_EQ_INT(AUTOMATION_CONFIG_BAD_CONFIG,
                automation_import_config(rules_blob, rules_length));
    TEST_EQ_INT(2, automation_rule_count());
    TEST_EQ_INT(SENSOR_OK, sensor_manager_add("lux", SENSOR_KIND_ADC1, 34));

    uint8_t tiny_rules[2];
    automation_config_status_t tiny_rule_status = AUTOMATION_CONFIG_OK;
    TEST_EQ_INT(0, automation_export_config(tiny_rules, sizeof(tiny_rules),
                                             &tiny_rule_status));
    TEST_EQ_INT(AUTOMATION_CONFIG_NO_SPACE, tiny_rule_status);

    TEST_CASE("imported rules are operational through the live sensor layer");
    TEST_EQ_INT(AUTOMATION_CONFIG_OK,
                automation_import_config(disabled_blob, disabled_length));
    automation_rule_list(rules, AUTOMATION_MAX_RULES);
    TEST_ASSERT(!rules[1].enabled);
    sensor_manager_test_set_adc_raw(34, 0);
    TEST_EQ_INT(PM_OK, pin_manager_output(23, false, PM_ORIGIN_LOCAL, "manual",
                                          1.0f, "test"));
    TEST_EQ_INT(0, automation_update(1100));
    TEST_ASSERT(!pin_on(23));
    sensor_manager_test_set_adc_raw(34, 3000);
    automation_set_enabled(true);
    TEST_EQ_INT(1, automation_update(1200));
    TEST_ASSERT(pin_on(23));
    automation_clear();

    TEST_CASE("empty configurations still serialize deterministically");
    TEST_EQ_INT(SENSOR_OK, sensor_manager_reset());
    sensor_status_t empty_sensor = SENSOR_ERR_INVALID_ARGUMENT;
    TEST_EQ_INT(3, sensor_manager_export_config(sensors_blob, sizeof(sensors_blob),
                                                &empty_sensor));
    TEST_EQ_INT(SENSOR_OK, empty_sensor);
    TEST_EQ_INT(0, sensors_blob[2]);

    automation_config_status_t empty_rule = AUTOMATION_CONFIG_BAD_CONFIG;
    TEST_EQ_INT(3, automation_export_config(rules_blob, sizeof(rules_blob),
                                            &empty_rule));
    TEST_EQ_INT(AUTOMATION_CONFIG_OK, empty_rule);
    TEST_EQ_INT(0, rules_blob[2]);
}
