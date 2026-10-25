#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deterministic natural-language pre-filter.
 *
 * This module runs before the TinyML model and does not depend on it. It
 * normalizes ASCII text, finds explicit GPIO numbers, and rejects constructs
 * that must never actuate hardware: negations, hypotheticals, prompt-injection
 * / role-play text, and pin reconfiguration requests.
 *
 * The intent classifier only decides what the user probably wants. This layer
 * decides whether the request is safe enough to hand to the pin manager.
 */

#define ENTITY_TEXT_MAX 160
#define ENTITY_MAX_TOKENS 24
#define ENTITY_MAX_TOKEN_CHARS 24
#define ENTITY_MAX_PINS 16

typedef enum {
    ENTITY_SAFE = 0,
    ENTITY_REJECT_EMPTY,
    ENTITY_REJECT_TOO_LONG,
    ENTITY_REJECT_NON_ASCII,
    ENTITY_REJECT_TOO_MANY_TOKENS,
    ENTITY_REJECT_TOKEN_TOO_LONG,
    ENTITY_REJECT_NEGATION,
    ENTITY_REJECT_HYPOTHETICAL,
    ENTITY_REJECT_INJECTION,
    ENTITY_REJECT_CONFIGURATION,
} entity_safety_t;

typedef struct {
    char text[ENTITY_TEXT_MAX + 1];
    char tokens[ENTITY_MAX_TOKENS][ENTITY_MAX_TOKEN_CHARS + 1];
    uint8_t token_count;
    uint8_t explicit_pins[ENTITY_MAX_PINS];
    uint8_t explicit_pin_count;
    bool mentions_all;
    bool mentions_any;
    bool mentions_confirm;
    entity_safety_t safety;
} entity_set_t;

/* Normalizes and classifies safety. Always fills out (on success or rejection)
 * with whatever was parsed before the rejection. */
entity_safety_t entity_extract(const char *text, entity_set_t *out);

bool entity_has_token(const entity_set_t *set, const char *token);
bool entity_has_sequence(const entity_set_t *set, uint8_t start,
                         const char *const *tokens, size_t count);
bool entity_has_pin(const entity_set_t *set, uint8_t pin);

const char *entity_safety_name(entity_safety_t safety);

#ifdef __cplusplus
}
#endif
