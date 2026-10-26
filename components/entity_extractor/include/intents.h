#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Canonical MVP intent families derived from intents.def.
 * Single source of truth for both C firmware and Python tools.
 */
typedef enum {
#define INTENT_DEF(id, str) INTENT_##id,
#include "intents.def"
#undef INTENT_DEF
    INTENT_COUNT
} intent_t;

#define INTENT_CLASS_COUNT ((size_t)INTENT_COUNT)

/* Convert intent enum to canonical uppercase string */
const char *intent_to_string(intent_t intent);

/* Parse string to intent enum, returns INTENT_UNKNOWN if not found */
intent_t intent_from_string(const char *name);

/* Check if an intent string is a valid MVP intent name */
bool intent_is_valid_name(const char *name);

/* Get intent name by index (0 to INTENT_COUNT - 1). Returns NULL if out of bounds. */
const char *intent_get_by_index(size_t index);

#ifdef __cplusplus
}
#endif
