#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "PinManager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only device view over the validated pin-manager registry. The pin
 * manager remains the single owner of names, pins and safety policy. */

typedef enum {
    DEVICE_KIND_OUTPUT = 0,
    DEVICE_KIND_INPUT = 1,
} device_kind_t;

typedef struct {
    uint8_t pin;
    device_kind_t kind;
    bool ai_allowed;
    bool active_high;
    char name[PM_NAME_MAX];
    uint8_t alias_count;
    char aliases[PM_MAX_ALIASES][PM_NAME_MAX];
} device_info_t;

size_t device_registry_list(device_info_t *out, size_t capacity, bool only_ai_allowed);
bool device_registry_resolve(const char *name, uint8_t *pin);
bool device_registry_output_exists(uint8_t pin);
const char *device_registry_kind_name(device_kind_t kind);

#ifdef __cplusplus
}
#endif
