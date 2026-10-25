#include "test_framework.h"

void test_entity(void);
void test_tinyml(void);
void test_gpio_driver(void);
void test_pin_manager(void);
void test_command_engine(void);
void test_sensor_automation(void);
void test_console(void);
void test_serialization(void);

int g_test_assertions = 0;
int g_test_failures = 0;
const char *g_test_case = "";

int main(void)
{
    printf("ESP32 TinyAI Controller host tests\n");
    printf("----------------------------------\n");

    test_entity();
    test_tinyml();
    test_gpio_driver();
    test_pin_manager();
    test_command_engine();
    test_sensor_automation();
    test_console();
    test_serialization();

    printf("----------------------------------\n");
    if (g_test_failures == 0) {
        printf("PASS: %d assertions\n", g_test_assertions);
        return 0;
    }
    printf("FAIL: %d failure(s) across %d assertions\n", g_test_failures,
           g_test_assertions);
    return 1;
}
