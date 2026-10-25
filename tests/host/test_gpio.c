#include "test_framework.h"

#include "GPIO_Driver.h"
#include "GPIO_Sim.h"

void test_gpio_driver(void)
{
    TEST_CASE("GPIO driver rejects reserved, absent and input-only misuse");
    GPIO_TestResetAll();
    uint8_t value = 0;

    TEST_EQ_INT(GPIO_OK, GPIO_Init(23, GPIO_MODE_OUTPUT));
    TEST_EQ_INT(GPIO_ERR_ALREADY_INITIALIZED, GPIO_Init(23, GPIO_MODE_OUTPUT));
    TEST_EQ_INT(GPIO_ERR_NOT_INITIALIZED, GPIO_Write(18, GPIO_LEVEL_HIGH));
    TEST_EQ_INT(GPIO_ERR_RESERVED, GPIO_Init(6, GPIO_MODE_OUTPUT));
    TEST_EQ_INT(GPIO_ERR_RESERVED, GPIO_Init(0, GPIO_MODE_INPUT));
    TEST_EQ_INT(GPIO_ERR_RESERVED, GPIO_Init(20, GPIO_MODE_INPUT));
    TEST_EQ_INT(GPIO_ERR_UNSUPPORTED_MODE, GPIO_Init(34, GPIO_MODE_OUTPUT));
    TEST_EQ_INT(GPIO_ERR_UNSUPPORTED_PULL, GPIO_Init(34, GPIO_MODE_INPUT_PULLUP));
    TEST_EQ_INT(GPIO_ERR_UNSUPPORTED_PULL,
                GPIO_InitFull(23, GPIO_MODE_OUTPUT, GPIO_PULL_UP, GPIO_LEVEL_LOW));

    GPIO_Capabilities caps;
    TEST_EQ_INT(GPIO_OK, GPIO_GetCapabilities(34, &caps));
    TEST_ASSERT(caps.exists);
    TEST_ASSERT(caps.input_capable);
    TEST_ASSERT(!caps.output_capable);
    TEST_ASSERT(!caps.pullup_capable);
    TEST_EQ_INT(GPIO_OK, GPIO_GetCapabilities(23, &caps));
    TEST_ASSERT(caps.output_capable);
    TEST_ASSERT(caps.pullup_capable);
    TEST_EQ_INT(GPIO_OK, GPIO_Read(23, &value));
    TEST_EQ_INT(0, value);

    TEST_CASE("GPIO output uses W1TS/W1TC and reads back its latch");
    TEST_EQ_INT(GPIO_OK, GPIO_Write(23, GPIO_LEVEL_HIGH));
    TEST_EQ_INT(1, GPIO_SimGetOutputLatch(23));
    TEST_ASSERT(GPIO_SimGetOutputEnable(23));
    TEST_EQ_INT(1u << 23, g_gpio_sim.last_w1ts_mask & (1u << 23));
    TEST_EQ_INT(1, g_gpio_sim.w1ts_writes);
    TEST_EQ_INT(GPIO_OK, GPIO_Read(23, &value));
    TEST_EQ_INT(1, value);
    TEST_EQ_INT(GPIO_OK, GPIO_Toggle(23));
    TEST_EQ_INT(0, GPIO_SimGetOutputLatch(23));
    TEST_EQ_INT(GPIO_OK, GPIO_Toggle(23));
    TEST_EQ_INT(1, GPIO_SimGetOutputLatch(23));

    TEST_CASE("GPIO active-low output latches HIGH before enabling the pad");
    GPIO_TestResetAll();
    TEST_EQ_INT(GPIO_OK,
                GPIO_InitFull(22, GPIO_MODE_OUTPUT, GPIO_PULL_NONE, GPIO_LEVEL_HIGH));
    TEST_ASSERT(GPIO_SimGetOutputLatch(22));
    TEST_ASSERT(GPIO_SimGetOutputEnable(22));
    TEST_ASSERT(g_gpio_sim.last_latch_seq < g_gpio_sim.last_enable_seq);
    TEST_EQ_INT(1, g_gpio_sim.last_enable_seq - g_gpio_sim.last_latch_seq);

    TEST_CASE("GPIO input pulls, pad reads and deinit are explicit");
    GPIO_TestResetAll();
    TEST_EQ_INT(GPIO_OK, GPIO_Init(27, GPIO_MODE_INPUT_PULLUP));
    TEST_ASSERT(GPIO_SimMuxSelectedGpio(27));
    TEST_EQ_INT(1, GPIO_SimGetPull(27));
    GPIO_SimSetInput(27, true);
    TEST_EQ_INT(GPIO_OK, GPIO_Read(27, &value));
    TEST_EQ_INT(1, value);
    TEST_EQ_INT(GPIO_ERR_NOT_OUTPUT, GPIO_Write(27, GPIO_LEVEL_HIGH));
    TEST_EQ_INT(GPIO_OK, GPIO_SetPull(27, GPIO_PULL_DOWN));
    TEST_EQ_INT(2, GPIO_SimGetPull(27));
    TEST_EQ_INT(GPIO_OK, GPIO_SetPull(27, GPIO_PULL_NONE));
    TEST_EQ_INT(0, GPIO_SimGetPull(27));
    TEST_EQ_INT(GPIO_OK, GPIO_DeInit(27));
    TEST_EQ_INT(GPIO_ERR_NOT_INITIALIZED, GPIO_Read(27, &value));
    TEST_EQ_INT(GPIO_OK, GPIO_DeInit(27));

    TEST_CASE("GPIO mask writes handle GPIO32/33 and reject mixed masks");
    GPIO_TestResetAll();
    TEST_EQ_INT(GPIO_OK, GPIO_Init(33, GPIO_MODE_OUTPUT));
    TEST_EQ_INT(GPIO_OK, GPIO_Init(25, GPIO_MODE_OUTPUT));
    TEST_EQ_INT(GPIO_OK,
                GPIO_WriteMasks((1ULL << 33) | (1ULL << 25), 0));
    TEST_ASSERT(GPIO_SimGetOutputLatch(33));
    TEST_ASSERT(GPIO_SimGetOutputLatch(25));
    TEST_EQ_INT(1, g_gpio_sim.out1_w1ts_writes);
    TEST_EQ_INT(1, g_gpio_sim.w1ts_writes);
    TEST_EQ_INT(GPIO_ERR_CONFLICT,
                GPIO_WriteMasks(1ULL << 25, 1ULL << 25));
    TEST_EQ_INT(GPIO_ERR_NOT_INITIALIZED, GPIO_WriteMasks(1ULL << 27, 0));

    TEST_CASE("GPIO reserved helper and status names are stable");
    TEST_ASSERT(GPIO_IsReserved(6));
    TEST_ASSERT(GPIO_IsReserved(20));
    TEST_ASSERT(!GPIO_IsReserved(23));
    TEST_EQ_STR("ok", GPIO_StatusName(GPIO_OK));
    TEST_EQ_STR("board-blocked", GPIO_StatusName(GPIO_ERR_RESERVED));
    TEST_EQ_STR("output", GPIO_ModeName(GPIO_MODE_OUTPUT));
    TEST_EQ_STR("up", GPIO_PullName(GPIO_PULL_UP));
}
