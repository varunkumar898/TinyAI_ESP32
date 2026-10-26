#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intents.h"
#include "typed_command.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EXTRACTOR_MAX_TEXT_LEN 256
#define EXTRACTOR_MAX_TOKENS 32
#define EXTRACTOR_MAX_TOKEN_CHARS 32
#define EXTRACTOR_MAX_DURATION_MS 3600000u /* 1 hour */
#define EXTRACTOR_MAX_LEDC_FREQ 40000000u  /* 40 MHz */
#define EXTRACTOR_MIN_LEDC_FREQ 1u         /* 1 Hz */

/* Safety filter results */
typedef enum {
    SAFETY_PASS = 0,
    SAFETY_REJECT_EMPTY,
    SAFETY_REJECT_TOO_LONG,
    SAFETY_REJECT_NON_ASCII,
    SAFETY_REJECT_NEGATION,
    SAFETY_REJECT_QUESTION,
    SAFETY_REJECT_INJECTION,
    SAFETY_REJECT_MULTI_COMMAND,
    SAFETY_REJECT_CONFIGURATION,
} safety_result_t;

/* Slot extractor status */
typedef enum {
    SLOT_STATUS_OK = 0,
    SLOT_STATUS_MISSING,
    SLOT_STATUS_INVALID,
    SLOT_STATUS_AMBIGUOUS,
    SLOT_STATUS_SAFETY_REJECTED,
} slot_status_t;

typedef struct {
    safety_result_t safety;
    char reason[80];
} safety_check_t;

typedef struct {
    slot_status_t status;
    char slot_name[32];   /* e.g. "pin", "duration", "frequency", "duty", "level", "payload" */
    char reason[80];      /* e.g. "missing required slot", "out of range (0-100)", "overflow" */
    typed_command_t command;
} slot_result_t;

/* Normalization outputs */
typedef struct {
    char normalized[EXTRACTOR_MAX_TEXT_LEN + 1];
    char masked[EXTRACTOR_MAX_TEXT_LEN + 1];
    safety_result_t safety;
    char safety_reason[80];
} extractor_normalized_t;

/*
 * Normalization:
 * - Lowercases ASCII letters.
 * - Collapses whitespace and strips useless punctuation.
 * - Converts written numbers ("eighteen", "one thousand") to digits.
 * - Normalizes units: ms/milliseconds -> ms, s/sec/seconds -> s, Hz/kHz/MHz, %/percent -> %.
 * - Normalizes aliases: "pin 2", "gpio2", "GPIO 2", "io2" -> "gpio2".
 *   "built-in led" -> "gpio2".
 * - Produces normalized text AND masked text (<NUM>, <PIN>, <FREQ>, <DUR>, <PCT>).
 * Fixed-size bounded buffers only. Rejects inputs over length limit.
 */
bool extractor_normalize(const char *input, extractor_normalized_t *out);

/*
 * Rule-based safety filter:
 * Checks for negation, questions without action, injections, and multi-command chaining.
 */
safety_check_t extractor_check_safety(const char *normalized_text);

/*
 * Slot extractor:
 * Parses explicit intent and parameters from text.
 * Runs deterministic extraction for all 12 MVP families.
 * Returns slot_result_t with explicit status.
 */
slot_result_t extractor_parse_command(const char *input);

/*
 * Parse written number words ("eighteen", "one thousand five hundred twenty") into uint64_t.
 * Returns true on success, false on overflow or invalid text.
 */
bool extractor_parse_written_number(const char *text, uint64_t *out_val);

/*
 * String helpers for status enums.
 */
const char *safety_result_name(safety_result_t safety);
const char *slot_status_name(slot_status_t status);

#ifdef __cplusplus
}
#endif
