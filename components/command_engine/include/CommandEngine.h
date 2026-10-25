#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "PinManager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Natural-language command engine.
 *
 * Pipeline:
 *   1. deterministic safety/entity extraction (no model dependency)
 *   2. optional TinyML intent classification
 *   3. deterministic device/group/pin resolution
 *   4. pin-manager policy and actuation
 *
 * Actuation is never performed by the model. The model only supplies one of
 * seven intent labels plus confidence; every write is re-validated here and by
 * PinManager.
 */

#define CMD_MESSAGE_MAX 1024

typedef enum {
    CMD_OK = 0,
    CMD_REJECTED,
    CMD_CONFIRMATION_REQUIRED,
    CMD_AI_DISABLED,
    CMD_NO_TARGET,
    CMD_AMBIGUOUS,
    CMD_INVALID_ARGUMENT,
    CMD_BUSY,
} command_status_t;

typedef enum {
    CMD_INTENT_NONE = -1,
    CMD_INTENT_TURN_ON = 0,
    CMD_INTENT_TURN_OFF = 1,
    CMD_INTENT_TOGGLE = 2,
    CMD_INTENT_GET_STATUS = 3,
    CMD_INTENT_LIST_DEVICES = 4,
    CMD_INTENT_HELP = 5,
    CMD_INTENT_UNKNOWN = 6,
} command_intent_t;

typedef struct {
    command_status_t status;
    command_intent_t intent;
    float confidence;
    float margin;
    float threshold;
    bool rejected;
    bool guard_rejected;
    bool confirmation_pending;
    uint8_t targets[PM_MAX_PINS];
    size_t target_count;
    char message[CMD_MESSAGE_MAX];
} command_result_t;

void command_engine_init(bool ai_enabled);

bool command_engine_ai_available(void);
bool command_engine_ai_enabled(void);
void command_engine_set_ai_enabled(bool enabled);

/* Classify only: runs the deterministic guard + model, never actuates and
 * never creates a pending confirmation. */
command_status_t command_engine_classify(const char *text, command_result_t *result);

/* Full command path. origin=PM_ORIGIN_AI is used by the serial "do" command so
 * the same AI permission policy applies to console-entered natural language. */
command_status_t command_engine_handle_text(const char *text, pm_origin_t origin,
                                            command_result_t *result);

/* Executes a pending multi-target ON/TOGGLE plan. Intended for the trusted
 * local console only; never call this from the AI path. */
command_status_t command_engine_confirm(command_result_t *result);
void command_engine_cancel(void);
bool command_engine_confirmation_pending(void);

bool command_engine_model_available(void);
/* Copies model version/parameter facts into out. */
bool command_engine_model_info(char *out, size_t capacity);

/* Measures this device's inference time when the model is compiled in.
 * Returns false if AI is unavailable. The serial console reports the measured
 * numbers; none are hard-coded. */
bool command_engine_benchmark(unsigned iterations, uint32_t *elapsed_us,
                              uint32_t *predictions);

const char *command_status_name(command_status_t status);
const char *command_intent_name(command_intent_t intent);

#ifdef COMMAND_ENGINE_HOST_TEST
void command_engine_test_set_time(uint32_t now_ms);
#endif

#ifdef __cplusplus
}
#endif
