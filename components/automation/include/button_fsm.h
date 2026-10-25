#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUTTON_DEBOUNCE_MS 30u
#define BUTTON_COOLDOWN_MS 250u
#define PIR_DEBOUNCE_MS 50u

typedef struct {
    bool initialized;
    bool stable_active;
    bool last_raw;
    uint32_t last_change_ms;
    uint32_t last_press_ms;
} button_fsm_t;

typedef struct {
    bool initialized;
    bool stable_active;
    bool last_raw;
    uint32_t last_change_ms;
} pir_fsm_t;

/* raw_active is already polarity-corrected: true means pressed/motion. */
void button_fsm_init(button_fsm_t *fsm, bool raw_active, uint32_t now_ms);
bool button_fsm_update(button_fsm_t *fsm, bool raw_active, uint32_t now_ms);

void pir_fsm_init(pir_fsm_t *fsm, bool raw_active, uint32_t now_ms);
bool pir_fsm_update(pir_fsm_t *fsm, bool raw_active, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
