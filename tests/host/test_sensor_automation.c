#include "test_framework.h"

#include "Automation.h"
#include "GPIO_Sim.h"
#include "PinManager.h"
#include "SensorManager.h"

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static bool pin_on(uint8_t pin)
{
    pm_pin_status_t status;
    if (pin_manager_get_status(pin, &status) != PM_OK) {
        return false;
    }
    return status.logical_state;
}

static void configure_pin(uint8_t pin, const char *name, GPIO_Mode mode,
                          GPIO_Pull pull, bool ai_allowed)
{
    TEST_EQ_INT(PM_OK, pin_manager_pin_init(pin, mode, pull));
    TEST_EQ_INT(PM_OK, pin_manager_assign(pin, name, true, ai_allowed, false));
}

void test_sensor_automation(void)
{
    pin_manager_reset();
    GPIO_TestResetAll();
    TEST_EQ_INT(PM_OK, pin_manager_init());
    TEST_EQ_INT(PM_OK, pin_manager_reset());
    sensor_manager_init();
    TEST_EQ_INT(SENSOR_OK, sensor_manager_reset());
    automation_init();
    automation_clear();
    automation_set_enabled(false);

    configure_pin(23, "led1", GPIO_MODE_OUTPUT, GPIO_PULL_NONE, true);
    configure_pin(25, "led2", GPIO_MODE_OUTPUT, GPIO_PULL_NONE, true);
    configure_pin(18, "fan", GPIO_MODE_OUTPUT, GPIO_PULL_NONE, false);
    configure_pin(27, "motion", GPIO_MODE_INPUT, GPIO_PULL_UP, false);
    configure_pin(34, "lux", GPIO_MODE_INPUT, GPIO_PULL_NONE, false);
    configure_pin(32, "adc_pulled", GPIO_MODE_INPUT, GPIO_PULL_UP, false);

    TEST_CASE("sensor manager validates kind, direction and pull configuration");
    TEST_EQ_INT(SENSOR_OK, sensor_manager_add("motion", SENSOR_KIND_DIGITAL, 27));
    TEST_EQ_INT(SENSOR_OK, sensor_manager_add("lux", SENSOR_KIND_ADC1, 34));
    TEST_EQ_INT(SENSOR_ERR_NAME_CONFLICT,
                sensor_manager_add("MOTION", SENSOR_KIND_DIGITAL, 27));
    TEST_EQ_INT(SENSOR_ERR_NAME_INVALID,
                sensor_manager_add("9bad", SENSOR_KIND_DIGITAL, 27));
    TEST_EQ_INT(SENSOR_ERR_INVALID_ARGUMENT,
                sensor_manager_add("bad", (sensor_kind_t)7, 27));
    TEST_EQ_INT(SENSOR_ERR_NOT_INPUT,
                sensor_manager_add("output", SENSOR_KIND_DIGITAL, 23));
    TEST_EQ_INT(SENSOR_ERR_PIN_NOT_READY,
                sensor_manager_add("missing", SENSOR_KIND_DIGITAL, 26));
    TEST_EQ_INT(SENSOR_ERR_NOT_ADC1,
                sensor_manager_add("notadc", SENSOR_KIND_ADC1, 27));
    TEST_EQ_INT(SENSOR_ERR_ADC_USES_PULL,
                sensor_manager_add("pulled", SENSOR_KIND_ADC1, 32));

    sensor_sample_t samples[SENSOR_MAX];
    TEST_EQ_INT(2, sensor_manager_list(samples, SENSOR_MAX));
    TEST_EQ_INT(SENSOR_ERR_NOT_FOUND, sensor_manager_get("missing", &samples[0]));
    TEST_EQ_INT(SENSOR_ERR_INVALID_ARGUMENT, sensor_manager_get("motion", NULL));

    TEST_CASE("digital sensor samples follow the pin manager input");
    sensor_sample_t sample;
    GPIO_SimSetInput(27, false);
    TEST_EQ_INT(SENSOR_OK, sensor_manager_sample("motion", &sample));
    TEST_ASSERT(sample.valid);
    TEST_EQ_INT(0, sample.raw);
    TEST_EQ_INT(0, sample.value);
    GPIO_SimSetInput(27, true);
    TEST_EQ_INT(SENSOR_OK, sensor_manager_sample("motion", &sample));
    TEST_ASSERT(sample.valid);
    TEST_EQ_INT(1, sample.raw);
    TEST_EQ_INT(1, sample.value);
    TEST_ASSERT(!pin_on(23));

    TEST_CASE("ADC1 samples report raw counts and approximate millivolts");
    sensor_manager_test_set_adc_raw(34, 0);
    TEST_EQ_INT(SENSOR_OK, sensor_manager_sample("lux", &sample));
    TEST_EQ_INT(0, sample.raw);
    TEST_EQ_INT(0, sample.value);
    sensor_manager_test_set_adc_raw(34, 2048);
    TEST_EQ_INT(SENSOR_OK, sensor_manager_sample("lux", &sample));
    TEST_EQ_INT(2048, sample.raw);
    TEST_EQ_INT(550, sample.value);
    sensor_manager_test_set_adc_raw(34, 4095);
    TEST_EQ_INT(SENSOR_OK, sensor_manager_sample("lux", &sample));
    TEST_EQ_INT(4095, sample.raw);
    TEST_EQ_INT(1100, sample.value);

    TEST_CASE("rule creation validates names, sensors, outputs and limits");
    TEST_EQ_INT(AUTOMATION_RULE_INVALID_ARGUMENT,
                automation_rule_add("", "lux", 23, AUTOMATION_ABOVE, 100, 10));
    TEST_EQ_INT(AUTOMATION_RULE_INVALID_ARGUMENT,
                automation_rule_add("bad", "lux", 23, (automation_condition_t)9, 100,
                                    10));
    TEST_EQ_INT(AUTOMATION_RULE_INVALID_ARGUMENT,
                automation_rule_add("bad", "lux", 23, AUTOMATION_ABOVE,
                                    100000001, 10));
    TEST_EQ_INT(AUTOMATION_RULE_INVALID_ARGUMENT,
                automation_rule_add("bad", "lux", 23, AUTOMATION_ABOVE, 100,
                                    100000001u));
    TEST_EQ_INT(AUTOMATION_RULE_SENSOR_MISSING,
                automation_rule_add("bad", "unknown", 23, AUTOMATION_ABOVE, 100, 10));
    TEST_EQ_INT(AUTOMATION_RULE_OUTPUT_UNUSABLE,
                automation_rule_add("bad", "lux", 27, AUTOMATION_ABOVE, 100, 10));
    TEST_EQ_INT(AUTOMATION_RULE_OUTPUT_UNUSABLE,
                automation_rule_add("bad", "lux", 26, AUTOMATION_ABOVE, 100, 10));
    TEST_EQ_INT(AUTOMATION_RULE_OK,
                automation_rule_add("hot", "lux", 23, AUTOMATION_ABOVE, 600, 100));
    TEST_EQ_INT(AUTOMATION_RULE_NAME_CONFLICT,
                automation_rule_add("HOT", "lux", 25, AUTOMATION_ABOVE, 700, 10));
    TEST_EQ_INT(AUTOMATION_RULE_OK,
                automation_rule_add("cool", "motion", 25, AUTOMATION_ABOVE, 0, 0));

    automation_rule_info_t rules[AUTOMATION_MAX_RULES];
    TEST_EQ_INT(2, automation_rule_list(rules, AUTOMATION_MAX_RULES));
    TEST_EQ_INT(2, automation_rule_count());
    TEST_EQ_STR("hot", rules[0].name);
    TEST_ASSERT(rules[0].enabled);
    TEST_ASSERT(!rules[0].active);
    TEST_EQ_INT(23, rules[0].output_pin);

    TEST_EQ_INT(AUTOMATION_RULE_OK, automation_rule_remove("cool"));

    TEST_CASE("above-threshold rule applies hysteresis and owns only its output");
    automation_set_enabled(true);
    sensor_manager_test_set_adc_raw(34, 1000);
    TEST_EQ_INT(0, automation_update(1000));
    TEST_ASSERT(!pin_on(23));
    sensor_manager_test_set_adc_raw(34, 3000);
    TEST_EQ_INT(1, automation_update(1100));
    TEST_ASSERT(pin_on(23));

    automation_set_enabled(true);
    sensor_manager_test_set_adc_raw(34, 2500);
    TEST_EQ_INT(0, automation_update(1200));
    TEST_ASSERT(pin_on(23));

    sensor_manager_test_set_adc_raw(34, 3000);
    TEST_EQ_INT(0, automation_update(1300));
    TEST_ASSERT(pin_on(23));
    automation_rule_list(rules, AUTOMATION_MAX_RULES);
    TEST_ASSERT(rules[0].active);
    TEST_ASSERT(rules[0].output_owned);
    TEST_EQ_INT(805, rules[0].last_value);

    sensor_manager_test_set_adc_raw(34, 2500);
    TEST_EQ_INT(0, automation_update(1400));
    TEST_ASSERT(pin_on(23));

    sensor_manager_test_set_adc_raw(34, 2000);
    TEST_EQ_INT(0, automation_update(1500));
    TEST_ASSERT(pin_on(23));

    sensor_manager_test_set_adc_raw(34, 1500);
    TEST_EQ_INT(1, automation_update(1600));
    TEST_ASSERT(!pin_on(23));

    sensor_manager_test_set_adc_raw(34, 2234);
    TEST_EQ_INT(1, automation_update(1700));
    TEST_ASSERT(pin_on(23));

    TEST_CASE("pausing or resetting a rule leaves its output unchanged");
    TEST_EQ_INT(AUTOMATION_RULE_OK, automation_rule_set_enabled("hot", false));
    TEST_ASSERT(pin_on(23));
    sensor_manager_test_set_adc_raw(34, 0);
    TEST_EQ_INT(0, automation_update(1800));
    TEST_ASSERT(pin_on(23));
    TEST_EQ_INT(AUTOMATION_RULE_OK, automation_rule_set_enabled("hot", true));
    TEST_EQ_INT(1, automation_update(1900));
    TEST_ASSERT(!pin_on(23));

    sensor_manager_test_set_adc_raw(34, 3000);
    TEST_EQ_INT(1, automation_update(2000));
    TEST_ASSERT(pin_on(23));
    TEST_EQ_INT(AUTOMATION_RULE_OK, automation_rule_reset("hot"));
    TEST_ASSERT(pin_on(23));
    automation_rule_list(rules, AUTOMATION_MAX_RULES);
    TEST_ASSERT(!rules[0].active);
    TEST_ASSERT(!rules[0].output_owned);
    TEST_EQ_INT(1, automation_update(2100));
    TEST_ASSERT(pin_on(23));

    TEST_CASE("global pause stops evaluation without writing outputs");
    automation_set_enabled(false);
    sensor_manager_test_set_adc_raw(34, 0);
    TEST_EQ_INT(0, automation_update(2200));
    TEST_ASSERT(pin_on(23));
    automation_set_enabled(true);

    TEST_CASE("missing sensors raise a rule fault without changing the output");
    TEST_EQ_INT(SENSOR_OK, sensor_manager_remove("lux"));
    TEST_EQ_INT(0, automation_update(2300));
    TEST_ASSERT(pin_on(23));
    automation_rule_list(rules, AUTOMATION_MAX_RULES);
    TEST_ASSERT(rules[0].fault);
    TEST_EQ_INT(SENSOR_OK, sensor_manager_add("lux", SENSOR_KIND_ADC1, 34));
    sensor_manager_test_set_adc_raw(34, 3000);
    TEST_EQ_INT(0, automation_update(2400));
    automation_rule_list(rules, AUTOMATION_MAX_RULES);
    TEST_ASSERT(!rules[0].fault);

    TEST_CASE("rule configuration round-trips without restoring runtime state");
    TEST_EQ_INT(AUTOMATION_RULE_OK,
                automation_rule_add("cool", "motion", 25, AUTOMATION_ABOVE, 0, 0));
    TEST_EQ_INT(AUTOMATION_RULE_OK, automation_rule_set_enabled("cool", false));
    uint8_t blob[512];
    automation_config_status_t export_status = AUTOMATION_CONFIG_BAD_CONFIG;
    size_t length = automation_export_config(blob, sizeof(blob), &export_status);
    TEST_EQ_INT(AUTOMATION_CONFIG_OK, export_status);
    TEST_ASSERT(length > 5);
    TEST_EQ_INT(AUTOMATION_CONFIG_SCHEMA_VERSION, read_u16(blob));
    TEST_EQ_INT(2, blob[2]);

    automation_clear();
    TEST_EQ_INT(0, automation_rule_count());
    TEST_ASSERT(pin_on(23));

    TEST_EQ_INT(AUTOMATION_CONFIG_OK, automation_import_config(blob, length));
    TEST_EQ_INT(2, automation_rule_count());
    automation_rule_list(rules, AUTOMATION_MAX_RULES);
    TEST_EQ_STR("hot", rules[0].name);
    TEST_EQ_STR("lux", rules[0].sensor);
    TEST_EQ_INT(AUTOMATION_ABOVE, rules[0].condition);
    TEST_EQ_INT(600, rules[0].threshold);
    TEST_EQ_INT(100, rules[0].hysteresis);
    TEST_ASSERT(rules[0].enabled);
    TEST_ASSERT(!rules[0].active);
    TEST_ASSERT(!rules[0].output_owned);
    TEST_EQ_STR("cool", rules[1].name);
    TEST_ASSERT(!rules[1].enabled);

    uint8_t bad[512];
    memcpy(bad, blob, length);
    bad[length - 1] ^= 0xff;
    TEST_EQ_INT(AUTOMATION_CONFIG_BAD_CONFIG,
                automation_import_config(bad, length));
    TEST_EQ_INT(AUTOMATION_CONFIG_BAD_CONFIG,
                automation_import_config(blob, length - 1));
    TEST_EQ_INT(AUTOMATION_CONFIG_BAD_CONFIG, automation_import_config(blob, 2));
    TEST_EQ_INT(AUTOMATION_CONFIG_BAD_CONFIG,
                automation_import_config(NULL, length));
    TEST_EQ_INT(2, automation_rule_count());

    uint8_t tiny[2];
    automation_config_status_t small_status = AUTOMATION_CONFIG_OK;
    TEST_EQ_INT(0, automation_export_config(tiny, sizeof(tiny), &small_status));
    TEST_EQ_INT(AUTOMATION_CONFIG_NO_SPACE, small_status);

    TEST_CASE("rule removal never writes the output");
    sensor_manager_test_set_adc_raw(34, 0);
    TEST_EQ_INT(PM_OK, pin_manager_output(23, false, PM_ORIGIN_LOCAL, "manual",
                                          1.0f, "test"));
    automation_update(2500);
    TEST_ASSERT(!pin_on(23));
    TEST_EQ_INT(PM_OK, pin_manager_output(23, true, PM_ORIGIN_LOCAL, "manual", 1.0f,
                                          "test"));
    TEST_EQ_INT(AUTOMATION_RULE_OK, automation_rule_remove("hot"));
    TEST_ASSERT(pin_on(23));
    TEST_EQ_INT(AUTOMATION_RULE_NOT_FOUND, automation_rule_remove("hot"));
    TEST_EQ_INT(1, automation_rule_count());

    TEST_CASE("PIR holds its output and releases after the configured hold");
    automation_clear();
    TEST_EQ_INT(PM_OK, pin_manager_output(23, false, PM_ORIGIN_LOCAL, "manual",
                                          1.0f, "test"));
    TEST_ASSERT(automation_bind_pir(27, true, 23, 500));
    automation_set_enabled(true);
    GPIO_SimSetInput(27, true);
    TEST_EQ_INT(1, automation_update(3000));
    TEST_ASSERT(pin_on(23));
    GPIO_SimSetInput(27, false);
    TEST_EQ_INT(0, automation_update(3050));
    TEST_ASSERT(pin_on(23));
    TEST_EQ_INT(0, automation_update(3110));
    TEST_ASSERT(pin_on(23));
    TEST_EQ_INT(1, automation_update(3611));
    TEST_ASSERT(!pin_on(23));

    TEST_CASE("rebinding PIR releases its previously owned output");
    GPIO_SimSetInput(27, true);
    TEST_EQ_INT(0, automation_update(4000));
    TEST_EQ_INT(1, automation_update(4051));
    TEST_ASSERT(pin_on(23));
    TEST_ASSERT(automation_bind_pir(27, true, 25, 500));
    TEST_ASSERT(!pin_on(23));

    TEST_CASE("button presses debounce, respect cooldown and toggle the output");
    automation_clear();
    automation_set_enabled(true);
    TEST_EQ_INT(PM_OK, pin_manager_output(25, false, PM_ORIGIN_LOCAL, "manual",
                                          1.0f, "test"));
    TEST_ASSERT(automation_bind_button(27, true, 25));
    GPIO_SimSetInput(27, false);
    TEST_EQ_INT(0, automation_update(5000));
    GPIO_SimSetInput(27, true);
    TEST_EQ_INT(0, automation_update(5100));
    TEST_EQ_INT(1, automation_update(5131));
    TEST_ASSERT(pin_on(25));
    GPIO_SimSetInput(27, false);
    TEST_EQ_INT(0, automation_update(5200));
    TEST_EQ_INT(0, automation_update(5240));
    GPIO_SimSetInput(27, true);
    TEST_EQ_INT(0, automation_update(5300));
    TEST_EQ_INT(0, automation_update(5340));
    TEST_ASSERT(pin_on(25));
    GPIO_SimSetInput(27, false);
    automation_update(5400);
    automation_update(5450);
    GPIO_SimSetInput(27, true);
    automation_update(5500);
    TEST_EQ_INT(1, automation_update(5550));
    TEST_ASSERT(!pin_on(25));

    TEST_CASE("automation_clear leaves outputs untouched");
    TEST_EQ_INT(PM_OK, pin_manager_output(25, true, PM_ORIGIN_LOCAL, "manual",
                                          1.0f, "test"));
    automation_clear();
    TEST_ASSERT(pin_on(25));
    TEST_EQ_INT(0, automation_rule_count());
    automation_status_t status;
    automation_get_status(&status);
    TEST_ASSERT(!status.pir_bound);
    TEST_ASSERT(!status.button_bound);
    TEST_ASSERT(!automation_is_enabled());

    TEST_CASE("status names are stable");
    TEST_EQ_STR("digital", sensor_kind_name(SENSOR_KIND_DIGITAL));
    TEST_EQ_STR("adc1", sensor_kind_name(SENSOR_KIND_ADC1));
    TEST_EQ_STR("name-conflict", sensor_status_name(SENSOR_ERR_NAME_CONFLICT));
    TEST_EQ_STR("sensor-missing",
                automation_rule_status_name(AUTOMATION_RULE_SENSOR_MISSING));
}
