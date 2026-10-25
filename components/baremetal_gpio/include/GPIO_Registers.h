#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Thin register access layer. On the ESP32 target this maps directly to the
 * Espressif SoC register definitions (no literal register addresses). On the
 * host, tests/host/gpio_reg_sim.c implements the same operations over a
 * simulated register file so the driver logic itself is tested.
 *
 * Plain GPIO is routed through IO_MUX function 2 (PIN_FUNC_GPIO). For outputs,
 * the GPIO matrix output selector is set to SIG_GPIO_OUT_IDX so no peripheral
 * signal drives the pad. Inputs are read directly from GPIO_IN/GPIO_IN1 after
 * enabling FUN_IE; the input matrix is for peripheral input signals and is not
 * used for a direct GPIO read.
 */

#if defined(ESP_PLATFORM)
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_sig_map.h"

static inline void GPIO_HW_SelectGpioFunction(uint8_t pin)
{
    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
    REG_CLR_BIT(GPIO_PIN_MUX_REG[pin], SLP_SEL);
}

static inline void GPIO_HW_SetInputEnable(uint8_t pin, bool enable)
{
    if (enable) {
        PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[pin]);
    } else {
        PIN_INPUT_DISABLE(GPIO_PIN_MUX_REG[pin]);
    }
}

static inline void GPIO_HW_SetPull(uint8_t pin, bool pull_up, bool pull_down)
{
    uint32_t reg = GPIO_PIN_MUX_REG[pin];
    if (pull_up) {
        REG_SET_BIT(reg, FUN_PU);
    } else {
        REG_CLR_BIT(reg, FUN_PU);
    }
    if (pull_down) {
        REG_SET_BIT(reg, FUN_PD);
    } else {
        REG_CLR_BIT(reg, FUN_PD);
    }
}

static inline void GPIO_HW_SetOutputMatrixGpio(uint8_t pin)
{
    REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + ((uint32_t)pin * 4u), SIG_GPIO_OUT_IDX);
}

static inline void GPIO_HW_WriteOutputMask(uint64_t set_mask, uint64_t clear_mask)
{
    uint32_t set_low = (uint32_t)(set_mask & 0xFFFFFFFFu);
    uint32_t clear_low = (uint32_t)(clear_mask & 0xFFFFFFFFu);
    uint32_t set_high = (uint32_t)((set_mask >> 32) & 0xFFu);
    uint32_t clear_high = (uint32_t)((clear_mask >> 32) & 0xFFu);
    if (set_low != 0) {
        REG_WRITE(GPIO_OUT_W1TS_REG, set_low);
    }
    if (clear_low != 0) {
        REG_WRITE(GPIO_OUT_W1TC_REG, clear_low);
    }
    if (set_high != 0) {
        REG_WRITE(GPIO_OUT1_W1TS_REG, set_high);
    }
    if (clear_high != 0) {
        REG_WRITE(GPIO_OUT1_W1TC_REG, clear_high);
    }
}

static inline void GPIO_HW_SetOutputEnableMask(uint64_t set_mask, uint64_t clear_mask)
{
    uint32_t set_low = (uint32_t)(set_mask & 0xFFFFFFFFu);
    uint32_t clear_low = (uint32_t)(clear_mask & 0xFFFFFFFFu);
    uint32_t set_high = (uint32_t)((set_mask >> 32) & 0xFFu);
    uint32_t clear_high = (uint32_t)((clear_mask >> 32) & 0xFFu);
    if (set_low != 0) {
        REG_WRITE(GPIO_ENABLE_W1TS_REG, set_low);
    }
    if (clear_low != 0) {
        REG_WRITE(GPIO_ENABLE_W1TC_REG, clear_low);
    }
    if (set_high != 0) {
        REG_WRITE(GPIO_ENABLE1_W1TS_REG, set_high);
    }
    if (clear_high != 0) {
        REG_WRITE(GPIO_ENABLE1_W1TC_REG, clear_high);
    }
}

static inline uint32_t GPIO_HW_ReadInputBits(void)
{
    return REG_READ(GPIO_IN_REG);
}

static inline uint32_t GPIO_HW_ReadInputBitsHigh(void)
{
    return REG_READ(GPIO_IN1_REG) & 0xFFu;
}

static inline uint32_t GPIO_HW_ReadOutputBits(void)
{
    return REG_READ(GPIO_OUT_REG);
}

static inline uint32_t GPIO_HW_ReadOutputBitsHigh(void)
{
    return REG_READ(GPIO_OUT1_REG) & 0xFFu;
}

#else /* host simulated register backend */

void GPIO_HW_SelectGpioFunction(uint8_t pin);
void GPIO_HW_SetInputEnable(uint8_t pin, bool enable);
void GPIO_HW_SetPull(uint8_t pin, bool pull_up, bool pull_down);
void GPIO_HW_SetOutputMatrixGpio(uint8_t pin);
void GPIO_HW_WriteOutputMask(uint64_t set_mask, uint64_t clear_mask);
void GPIO_HW_SetOutputEnableMask(uint64_t set_mask, uint64_t clear_mask);
uint32_t GPIO_HW_ReadInputBits(void);
uint32_t GPIO_HW_ReadInputBitsHigh(void);
uint32_t GPIO_HW_ReadOutputBits(void);
uint32_t GPIO_HW_ReadOutputBitsHigh(void);

#endif
