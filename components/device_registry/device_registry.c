#include "device_registry.h"

#include <string.h>

size_t device_registry_list(device_info_t *out, size_t capacity, bool only_ai_allowed)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }
    size_t n = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
        pm_pin_status_t status;
        if (pin_manager_get_status(pin, &status) != PM_OK) {
            continue;
        }
        if (!status.configured) {
            continue;
        }
        if (only_ai_allowed && !status.ai_allowed) {
            continue;
        }
        if (n >= capacity) {
            break;
        }
        memset(&out[n], 0, sizeof(out[n]));
        out[n].pin = pin;
        out[n].kind = status.mode == GPIO_MODE_OUTPUT ? DEVICE_KIND_OUTPUT
                                                      : DEVICE_KIND_INPUT;
        out[n].ai_allowed = status.ai_allowed;
        out[n].active_high = status.active_high;
        strncpy(out[n].name, status.device_name, sizeof(out[n].name) - 1);
        out[n].alias_count = status.alias_count;
        for (uint8_t a = 0;
             a < status.alias_count && a < PM_MAX_ALIASES; ++a) {
            strncpy(out[n].aliases[a], status.aliases[a],
                    sizeof(out[n].aliases[a]) - 1);
        }
        ++n;
    }
    return n;
}

bool device_registry_resolve(const char *name, uint8_t *pin)
{
    return pin_manager_resolve_name(name, pin);
}

bool device_registry_output_exists(uint8_t pin)
{
    pm_pin_status_t status;
    return pin_manager_get_status(pin, &status) == PM_OK && status.configured &&
           status.mode == GPIO_MODE_OUTPUT;
}

const char *device_registry_kind_name(device_kind_t kind)
{
    switch (kind) {
    case DEVICE_KIND_OUTPUT: return "output";
    case DEVICE_KIND_INPUT: return "input";
    default: return "unknown";
    }
}
