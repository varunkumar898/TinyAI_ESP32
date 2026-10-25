#include "button_fsm.h"

#include <stddef.h>

void button_fsm_init(button_fsm_t *fsm, bool raw_active, uint32_t now_ms)
{
    if (fsm == NULL) {
        return;
    }
    fsm->initialized = true;
    fsm->stable_active = raw_active;
    fsm->last_raw = raw_active;
    fsm->last_change_ms = now_ms;
    fsm->last_press_ms = now_ms - BUTTON_COOLDOWN_MS;
}

bool button_fsm_update(button_fsm_t *fsm, bool raw_active, uint32_t now_ms)
{
    if (fsm == NULL) {
        return false;
    }
    if (!fsm->initialized) {
        button_fsm_init(fsm, raw_active, now_ms);
        return false;
    }
    if (raw_active != fsm->last_raw) {
        fsm->last_raw = raw_active;
        fsm->last_change_ms = now_ms;
        return false;
    }
    if (now_ms - fsm->last_change_ms < BUTTON_DEBOUNCE_MS) {
        return false;
    }
    if (fsm->stable_active == fsm->last_raw) {
        return false;
    }
    fsm->stable_active = fsm->last_raw;
    if (!fsm->stable_active) {
        return false;
    }
    if (now_ms - fsm->last_press_ms < BUTTON_COOLDOWN_MS) {
        return false;
    }
    fsm->last_press_ms = now_ms;
    return true;
}

void pir_fsm_init(pir_fsm_t *fsm, bool raw_active, uint32_t now_ms)
{
    if (fsm == NULL) {
        return;
    }
    fsm->initialized = true;
    fsm->stable_active = raw_active;
    fsm->last_raw = raw_active;
    fsm->last_change_ms = now_ms;
}

bool pir_fsm_update(pir_fsm_t *fsm, bool raw_active, uint32_t now_ms)
{
    if (fsm == NULL) {
        return false;
    }
    if (!fsm->initialized) {
        pir_fsm_init(fsm, raw_active, now_ms);
        return fsm->stable_active;
    }
    if (raw_active != fsm->last_raw) {
        fsm->last_raw = raw_active;
        fsm->last_change_ms = now_ms;
        return fsm->stable_active;
    }
    if (now_ms - fsm->last_change_ms < PIR_DEBOUNCE_MS) {
        return fsm->stable_active;
    }
    fsm->stable_active = fsm->last_raw;
    return fsm->stable_active;
}
