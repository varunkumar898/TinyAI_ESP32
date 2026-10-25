#pragma once

/*
 * Host-only simulated register backend for the bare-metal GPIO driver.
 * The target build never includes this file's implementation.
 *
 * The model mirrors the ESP32 registers used by GPIO_Registers.h rather than
 * mocking the public driver API, so tests exercise the same W1TS/W1TC,
 * IO_MUX and pad-driver logic as firmware.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef GPIO_DRIVER_HOST_TEST

#define GPIO_SIM_PIN_COUNT 40
#define GPIO_SIM_FUN_PD (1u << 7)
#define GPIO_SIM_FUN_PU (1u << 8)
#define GPIO_SIM_FUN_IE (1u << 9)
#define GPIO_SIM_MCU_SEL_SHIFT 12

typedef struct {
    uint32_t out;
    uint32_t out1;
    uint32_t enable;
    uint32_t enable1;
    uint32_t in;
    uint32_t in1;
    uint32_t io_mux[GPIO_SIM_PIN_COUNT];
    uint32_t func_out_sel[GPIO_SIM_PIN_COUNT];
    uint32_t pad_driver[GPIO_SIM_PIN_COUNT];
    uint8_t external_input[GPIO_SIM_PIN_COUNT];
    uint8_t latch_at_enable[GPIO_SIM_PIN_COUNT];

    uint32_t w1ts_writes;
    uint32_t w1tc_writes;
    uint32_t out1_w1ts_writes;
    uint32_t out1_w1tc_writes;
    uint32_t last_w1ts_mask;
    uint32_t last_w1tc_mask;
    uint32_t last_out1_w1ts_mask;
    uint32_t last_out1_w1tc_mask;
    uint32_t action_seq;
    uint32_t last_latch_seq;
    uint32_t last_enable_seq;
} gpio_sim_regs_t;

extern gpio_sim_regs_t g_gpio_sim;

void GPIO_SimReset(void);
void GPIO_SimSetInput(uint8_t pin, bool high);
bool GPIO_SimGetInput(uint8_t pin);
bool GPIO_SimGetOutputEnable(uint8_t pin);
bool GPIO_SimGetOutputLatch(uint8_t pin);
int GPIO_SimGetPull(uint8_t pin); /* 0=none, 1=up, 2=down */
bool GPIO_SimMuxSelectedGpio(uint8_t pin);

/* Resets both the register model and the driver's internal state. */
void GPIO_TestResetAll(void);

#endif
