/*
 * Host-only register/file simulator for the ESP32 bare-metal GPIO driver.
 *
 * The driver calls the same GPIO_HW_* functions in host builds that it calls
 * in firmware, so these tests exercise W1TS/W1TC semantics, IO_MUX selection,
 * pulls, latch-before-enable ordering and mask handling without a board.
 */

#include "GPIO_Sim.h"
#include "GPIO_Driver.h"

#include <string.h>

gpio_sim_regs_t g_gpio_sim;

static bool low_pin(uint8_t pin)
{
    return pin < 32u;
}

static uint32_t pin_bit(uint8_t pin)
{
    return 1u << (pin < 32u ? pin : (pin - 32u));
}

static bool output_enabled(uint8_t pin)
{
    return low_pin(pin) ? (g_gpio_sim.enable & pin_bit(pin)) != 0
                        : (g_gpio_sim.enable1 & pin_bit(pin)) != 0;
}

static bool output_latch(uint8_t pin)
{
    return low_pin(pin) ? (g_gpio_sim.out & pin_bit(pin)) != 0
                        : (g_gpio_sim.out1 & pin_bit(pin)) != 0;
}

static void recompute_input_pin(uint8_t pin)
{
    uint32_t bit = pin_bit(pin);
    bool high = output_enabled(pin) ? output_latch(pin)
                                    : (g_gpio_sim.external_input[pin] != 0);
    if (low_pin(pin)) {
        if (high) {
            g_gpio_sim.in |= bit;
        } else {
            g_gpio_sim.in &= ~bit;
        }
    } else {
        if (high) {
            g_gpio_sim.in1 |= bit;
        } else {
            g_gpio_sim.in1 &= ~bit;
        }
    }
}

static void recompute_inputs(void)
{
    for (uint8_t pin = 0; pin < GPIO_SIM_PIN_COUNT; ++pin) {
        recompute_input_pin(pin);
    }
}

void GPIO_SimReset(void)
{
    memset(&g_gpio_sim, 0, sizeof(g_gpio_sim));
    for (uint8_t pin = 0; pin < GPIO_SIM_PIN_COUNT; ++pin) {
        /* Reset state has every pad in the GPIO function, input disabled. */
        g_gpio_sim.io_mux[pin] = (2u << GPIO_SIM_MCU_SEL_SHIFT);
    }
}

void GPIO_SimSetInput(uint8_t pin, bool high)
{
    if (pin >= GPIO_SIM_PIN_COUNT) {
        return;
    }
    g_gpio_sim.external_input[pin] = high ? 1u : 0u;
    recompute_input_pin(pin);
}

bool GPIO_SimGetInput(uint8_t pin)
{
    if (pin >= GPIO_SIM_PIN_COUNT) {
        return false;
    }
    return low_pin(pin) ? (g_gpio_sim.in & pin_bit(pin)) != 0
                        : (g_gpio_sim.in1 & pin_bit(pin)) != 0;
}

bool GPIO_SimGetOutputEnable(uint8_t pin)
{
    return pin < GPIO_SIM_PIN_COUNT && output_enabled(pin);
}

bool GPIO_SimGetOutputLatch(uint8_t pin)
{
    return pin < GPIO_SIM_PIN_COUNT && output_latch(pin);
}

int GPIO_SimGetPull(uint8_t pin)
{
    if (pin >= GPIO_SIM_PIN_COUNT) {
        return 0;
    }
    uint32_t mux = g_gpio_sim.io_mux[pin];
    if ((mux & GPIO_SIM_FUN_PU) != 0) {
        return 1;
    }
    if ((mux & GPIO_SIM_FUN_PD) != 0) {
        return 2;
    }
    return 0;
}

bool GPIO_SimMuxSelectedGpio(uint8_t pin)
{
    if (pin >= GPIO_SIM_PIN_COUNT) {
        return false;
    }
    return ((g_gpio_sim.io_mux[pin] >> GPIO_SIM_MCU_SEL_SHIFT) & 0x7u) == 2u;
}

void GPIO_TestResetAll(void)
{
    GPIO_SimReset();
    GPIO_TestResetDriverState();
}

/* ---- GPIO_HW_* contract implemented by this host backend ---- */

void GPIO_HW_SelectGpioFunction(uint8_t pin)
{
    if (pin >= GPIO_SIM_PIN_COUNT) {
        return;
    }
    uint32_t mux = g_gpio_sim.io_mux[pin];
    mux &= ~(0x7u << GPIO_SIM_MCU_SEL_SHIFT);
    mux |= (2u << GPIO_SIM_MCU_SEL_SHIFT);
    mux &= ~(1u << 7); /* SLP_SEL */
    g_gpio_sim.io_mux[pin] = mux;
}

void GPIO_HW_SetInputEnable(uint8_t pin, bool enable)
{
    if (pin >= GPIO_SIM_PIN_COUNT) {
        return;
    }
    if (enable) {
        g_gpio_sim.io_mux[pin] |= GPIO_SIM_FUN_IE;
    } else {
        g_gpio_sim.io_mux[pin] &= ~GPIO_SIM_FUN_IE;
    }
}

void GPIO_HW_SetPull(uint8_t pin, bool pull_up, bool pull_down)
{
    if (pin >= GPIO_SIM_PIN_COUNT) {
        return;
    }
    uint32_t mux = g_gpio_sim.io_mux[pin];
    if (pull_up) {
        mux |= GPIO_SIM_FUN_PU;
    } else {
        mux &= ~GPIO_SIM_FUN_PU;
    }
    if (pull_down) {
        mux |= GPIO_SIM_FUN_PD;
    } else {
        mux &= ~GPIO_SIM_FUN_PD;
    }
    g_gpio_sim.io_mux[pin] = mux;
}

void GPIO_HW_SetOutputMatrixGpio(uint8_t pin)
{
    if (pin >= GPIO_SIM_PIN_COUNT) {
        return;
    }
    g_gpio_sim.func_out_sel[pin] = 128u; /* SIG_GPIO_OUT_IDX */
    g_gpio_sim.pad_driver[pin] = 1u;
}

void GPIO_HW_WriteOutputMask(uint64_t set_mask, uint64_t clear_mask)
{
    uint32_t set_low = (uint32_t)(set_mask & 0xffffffffu);
    uint32_t clear_low = (uint32_t)(clear_mask & 0xffffffffu);
    uint32_t set_high = (uint32_t)((set_mask >> 32) & 0xffu);
    uint32_t clear_high = (uint32_t)((clear_mask >> 32) & 0xffu);

    g_gpio_sim.action_seq++;
    if (set_low != 0) {
        g_gpio_sim.w1ts_writes++;
        g_gpio_sim.last_w1ts_mask = set_low;
        g_gpio_sim.out |= set_low;
        g_gpio_sim.last_latch_seq = g_gpio_sim.action_seq;
    }
    if (clear_low != 0) {
        g_gpio_sim.w1tc_writes++;
        g_gpio_sim.last_w1tc_mask = clear_low;
        g_gpio_sim.out &= ~clear_low;
        g_gpio_sim.last_latch_seq = g_gpio_sim.action_seq;
    }
    if (set_high != 0) {
        g_gpio_sim.out1_w1ts_writes++;
        g_gpio_sim.last_out1_w1ts_mask = set_high;
        g_gpio_sim.out1 |= set_high;
        g_gpio_sim.last_latch_seq = g_gpio_sim.action_seq;
    }
    if (clear_high != 0) {
        g_gpio_sim.out1_w1tc_writes++;
        g_gpio_sim.last_out1_w1tc_mask = clear_high;
        g_gpio_sim.out1 &= ~clear_high;
        g_gpio_sim.last_latch_seq = g_gpio_sim.action_seq;
    }
    recompute_inputs();
}

void GPIO_HW_SetOutputEnableMask(uint64_t set_mask, uint64_t clear_mask)
{
    uint32_t set_low = (uint32_t)(set_mask & 0xffffffffu);
    uint32_t clear_low = (uint32_t)(clear_mask & 0xffffffffu);
    uint32_t set_high = (uint32_t)((set_mask >> 32) & 0xffu);
    uint32_t clear_high = (uint32_t)((clear_mask >> 32) & 0xffu);

    g_gpio_sim.action_seq++;
    if (set_low != 0) {
        g_gpio_sim.enable |= set_low;
        g_gpio_sim.last_enable_seq = g_gpio_sim.action_seq;
        for (uint8_t pin = 0; pin < 32; ++pin) {
            if ((set_low & (1u << pin)) != 0) {
                g_gpio_sim.latch_at_enable[pin] = output_latch(pin) ? 1u : 0u;
            }
        }
    }
    if (clear_low != 0) {
        g_gpio_sim.enable &= ~clear_low;
        g_gpio_sim.last_enable_seq = g_gpio_sim.action_seq;
    }
    if (set_high != 0) {
        g_gpio_sim.enable1 |= set_high;
        g_gpio_sim.last_enable_seq = g_gpio_sim.action_seq;
        for (uint8_t pin = 32; pin < 40; ++pin) {
            if ((set_high & (1u << (pin - 32u))) != 0) {
                g_gpio_sim.latch_at_enable[pin] = output_latch(pin) ? 1u : 0u;
            }
        }
    }
    if (clear_high != 0) {
        g_gpio_sim.enable1 &= ~clear_high;
        g_gpio_sim.last_enable_seq = g_gpio_sim.action_seq;
    }
    recompute_inputs();
}

uint32_t GPIO_HW_ReadInputBits(void)
{
    return g_gpio_sim.in;
}

uint32_t GPIO_HW_ReadInputBitsHigh(void)
{
    return g_gpio_sim.in1 & 0xffu;
}

uint32_t GPIO_HW_ReadOutputBits(void)
{
    return g_gpio_sim.out;
}

uint32_t GPIO_HW_ReadOutputBitsHigh(void)
{
    return g_gpio_sim.out1 & 0xffu;
}
