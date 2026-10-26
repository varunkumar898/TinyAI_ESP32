#include "CommandEngine.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "PinManager.h"
#include "entity_extractor.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if defined(CONFIG_TINYAI_ENABLED)
#define CMD_AI_COMPILED 1
#endif
#include "esp_timer.h"
#else
#define CMD_AI_COMPILED 1
#endif

#if defined(CMD_AI_COMPILED)
#include "model_data.h"
#include "tinyml.h"
#endif

/*
 * Strict natural-language actuation policy.
 *
 * The model has its own evaluation thresholds (currently 0.50 confidence and
 * 0.10 margin). Because a false positive can energise real hardware, ON/OFF/
 * TOGGLE commands must clear higher thresholds in addition to every
 * deterministic guard and pin-manager permission check.
 */
#define CMD_ACTUATION_CONFIDENCE 0.65f
#define CMD_ACTUATION_MARGIN 0.15f
#define CMD_PENDING_TIMEOUT_MS 30000u
#define CMD_TARGET_LABEL_MAX 80

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} message_t;

typedef enum {
    TARGET_NONE = 0,
    TARGET_PINS,
    TARGET_GROUP,
    TARGET_ALL,
} target_kind_t;

typedef struct {
    target_kind_t kind;
    uint8_t pins[PM_MAX_PINS];
    size_t pin_count;
    char group[PM_GROUP_NAME_MAX];
    char label[CMD_TARGET_LABEL_MAX];
    bool ambiguous;
} resolved_target_t;

typedef enum {
    PENDING_NONE = 0,
    PENDING_PINS_ON,
    PENDING_PINS_TOGGLE,
    PENDING_GROUP_ON,
    PENDING_GROUP_TOGGLE,
    PENDING_ALL_ON,
} pending_kind_t;

typedef struct {
    pending_kind_t kind;
    uint8_t pins[PM_MAX_PINS];
    size_t pin_count;
    char group[PM_GROUP_NAME_MAX];
    pm_origin_t origin;
    command_intent_t intent;
    float confidence;
    uint32_t created_ms;
} pending_plan_t;

static pending_plan_t s_pending;
#if defined(CMD_AI_COMPILED)
static const tinyml_model_t *s_model;
#endif
static bool s_ai_enabled;
static bool s_initialized;
#ifndef ESP_PLATFORM
static uint32_t s_host_time_ms;
#endif

static uint32_t now_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000);
#else
    return s_host_time_ms;
#endif
}

static void message_init(message_t *message, command_result_t *result)
{
    message->buffer = result->message;
    message->capacity = sizeof(result->message);
    message->length = 0;
    result->message[0] = '\0';
}

static void message_appendf(message_t *message, const char *format, ...)
{
    if (message->length + 1 >= message->capacity) {
        return;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(message->buffer + message->length,
                            message->capacity - message->length, format, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    size_t added = (size_t)written;
    if (added >= message->capacity - message->length) {
        message->length = message->capacity - 1;
    } else {
        message->length += added;
    }
}

static void copy_text(char *dst, size_t capacity, const char *src)
{
    if (dst == NULL || capacity == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t length = strlen(src);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static bool is_light_token(const char *token)
{
    static const char *const words[] = {
        "light", "lights", "lamp", "lamps", "led", "leds", "bulb", "bulbs",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        if (strcmp(token, words[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool entity_mentions_lights(const entity_set_t *entities)
{
    for (uint8_t i = 0; i < entities->token_count; ++i) {
        if (is_light_token(entities->tokens[i])) {
            return true;
        }
    }
    return false;
}

/* Converts a configured name or alias into the same word-only representation
 * used by the entity extractor, so "desk-light" matches "desk light". */
static void spoken_name(const char *input, char *out, size_t capacity)
{
    size_t used = 0;
    bool pending_space = false;
    if (capacity == 0) {
        return;
    }
    out[0] = '\0';
    for (const char *p = input; p != NULL && *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            if (pending_space && used > 0 && used + 1 < capacity) {
                out[used++] = ' ';
            }
            pending_space = false;
            if (used + 1 < capacity) {
                out[used++] = (char)tolower(c);
            }
        } else if (used > 0) {
            pending_space = true;
        }
    }
    out[used] = '\0';
}

static void candidate_text(const entity_set_t *entities, uint8_t start,
                           uint8_t count, char *out, size_t capacity)
{
    size_t used = 0;
    if (capacity == 0) {
        return;
    }
    out[0] = '\0';
    for (uint8_t i = 0; i < count; ++i) {
        const char *token = entities->tokens[start + i];
        size_t length = strlen(token);
        if (used > 0 && used + 1 < capacity) {
            out[used++] = ' ';
        }
        if (used + length >= capacity) {
            length = capacity - used - 1;
        }
        memcpy(out + used, token, length);
        used += length;
        out[used] = '\0';
    }
}

typedef struct {
    bool found;
    bool ambiguous;
    int best_length;
    uint8_t best_pin;
    char best_name[PM_NAME_MAX];
    int group_length;
    char group_name[PM_GROUP_NAME_MAX];
} name_matches_t;

static void note_device_match(name_matches_t *matches, uint8_t pin,
                              const char *name, int length)
{
    if (length > matches->best_length) {
        matches->best_length = length;
        matches->best_pin = pin;
        copy_text(matches->best_name, sizeof(matches->best_name), name);
        matches->found = true;
    } else if (length == matches->best_length && pin != matches->best_pin) {
        matches->ambiguous = true;
    }
}

static void note_group_match(name_matches_t *matches, const char *name,
                             int length)
{
    if (length > matches->group_length) {
        matches->group_length = length;
        copy_text(matches->group_name, sizeof(matches->group_name), name);
    } else if (length == matches->group_length &&
               strcmp(matches->group_name, name) != 0) {
        matches->ambiguous = true;
    }
}

static void find_name_matches(const entity_set_t *entities, name_matches_t *matches)
{
    memset(matches, 0, sizeof(*matches));

    pm_group_status_t groups[PM_MAX_GROUPS];
    size_t group_count = pin_manager_list_groups(groups, PM_MAX_GROUPS);

    for (uint8_t start = 0; start < entities->token_count; ++start) {
        for (uint8_t count = 1; start + count <= entities->token_count; ++count) {
            char candidate[ENTITY_MAX_TOKENS * (ENTITY_MAX_TOKEN_CHARS + 1)];
            candidate_text(entities, start, count, candidate, sizeof(candidate));
            if (candidate[0] == '\0') {
                continue;
            }
            int length = (int)count;

            for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
                pm_pin_status_t status;
                if (pin_manager_get_status(pin, &status) != PM_OK) {
                    continue;
                }
                char spoken[PM_NAME_MAX * 2];
                if (status.device_name[0] != '\0') {
                    spoken_name(status.device_name, spoken, sizeof(spoken));
                    if (strcmp(candidate, spoken) == 0) {
                        note_device_match(matches, pin, status.device_name, length);
                    }
                }
                for (uint8_t alias = 0; alias < status.alias_count; ++alias) {
                    spoken_name(status.aliases[alias], spoken, sizeof(spoken));
                    if (strcmp(candidate, spoken) == 0) {
                        note_device_match(matches, pin, status.device_name, length);
                    }
                }
            }

            for (size_t i = 0; i < group_count; ++i) {
                char spoken[PM_GROUP_NAME_MAX * 2];
                spoken_name(groups[i].name, spoken, sizeof(spoken));
                if (strcmp(candidate, spoken) == 0) {
                    note_group_match(matches, groups[i].name, length);
                }
            }
        }
    }

    if (matches->found && matches->group_length > 0) {
        /* The same words could name both a group and a device. Never guess. */
        matches->ambiguous = true;
    }
    if (matches->found && matches->group_length > matches->best_length) {
        matches->ambiguous = true;
    }
}

static size_t group_pin_list(const char *group, uint8_t *out, size_t capacity)
{
    pm_group_status_t status;
    if (pin_manager_get_group(group, &status) != PM_OK) {
        return 0;
    }
    size_t count = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS && count < capacity; ++pin) {
        if ((status.pin_mask & (1ULL << pin)) != 0) {
            out[count++] = pin;
        }
    }
    return count;
}

static void set_target_label(resolved_target_t *target)
{
    target->label[0] = '\0';
    if (target->kind == TARGET_GROUP) {
        snprintf(target->label, sizeof(target->label), "group %s", target->group);
        return;
    }
    if (target->kind == TARGET_ALL) {
        snprintf(target->label, sizeof(target->label), "all outputs");
        return;
    }
    if (target->kind != TARGET_PINS || target->pin_count == 0) {
        snprintf(target->label, sizeof(target->label), "none");
        return;
    }
    if (target->pin_count == 1) {
        pm_pin_status_t status;
        if (pin_manager_get_status(target->pins[0], &status) == PM_OK &&
            status.device_name[0] != '\0') {
            snprintf(target->label, sizeof(target->label), "%s (GPIO%u)",
                     status.device_name, (unsigned)target->pins[0]);
        } else {
            snprintf(target->label, sizeof(target->label), "GPIO%u",
                     (unsigned)target->pins[0]);
        }
        return;
    }
    snprintf(target->label, sizeof(target->label), "%u outputs",
             (unsigned)target->pin_count);
}

static void resolve_target(const entity_set_t *entities, resolved_target_t *target)
{
    memset(target, 0, sizeof(*target));
    target->kind = TARGET_NONE;

    name_matches_t matches;
    find_name_matches(entities, &matches);

    bool explicit_pins = entities->explicit_pin_count > 0;
    for (uint8_t i = 0; i < entities->explicit_pin_count; ++i) {
        target->pins[target->pin_count++] = entities->explicit_pins[i];
    }

    if (explicit_pins && matches.found) {
        bool matches_one = false;
        for (uint8_t i = 0; i < entities->explicit_pin_count; ++i) {
            if (entities->explicit_pins[i] == matches.best_pin) {
                matches_one = true;
                break;
            }
        }
        if (!matches_one) {
            target->ambiguous = true;
        }
    }
    if (explicit_pins && matches.group_length > 0) {
        target->ambiguous = true;
    }
    if (explicit_pins && entities->mentions_all) {
        target->ambiguous = true;
    }
    if (matches.ambiguous) {
        target->ambiguous = true;
    }

    if (target->ambiguous) {
        target->kind = TARGET_NONE;
        return;
    }
    if (explicit_pins) {
        target->kind = TARGET_PINS;
        set_target_label(target);
        return;
    }
    if (matches.group_length > 0) {
        target->kind = TARGET_GROUP;
        copy_text(target->group, sizeof(target->group), matches.group_name);
        set_target_label(target);
        return;
    }
    if (matches.found) {
        target->kind = TARGET_PINS;
        target->pins[0] = matches.best_pin;
        target->pin_count = 1;
        set_target_label(target);
        return;
    }
    if (entities->mentions_all) {
        if (entity_mentions_lights(entities)) {
            /* "all lights" must refer to an explicitly configured group. Turning
             * off every output because an unrelated group was never created
             * would be surprising and unsafe. */
            target->kind = TARGET_NONE;
            return;
        }
        target->kind = TARGET_ALL;
        set_target_label(target);
        return;
    }

    target->kind = TARGET_NONE;
}

static bool tokens_end_with_conjunction(const entity_set_t *entities)
{
    if (entities->token_count == 0) {
        return false;
    }
    const char *last = entities->tokens[entities->token_count - 1];
    return strcmp(last, "and") == 0 || strcmp(last, "or") == 0 ||
           strcmp(last, "plus") == 0;
}

#if defined(CMD_AI_COMPILED)
static command_intent_t map_intent(tinyml_intent_t intent)
{
    switch (intent) {
    case TINYML_INTENT_TURN_ON: return CMD_INTENT_TURN_ON;
    case TINYML_INTENT_TURN_OFF: return CMD_INTENT_TURN_OFF;
    case TINYML_INTENT_TOGGLE: return CMD_INTENT_TOGGLE;
    case TINYML_INTENT_GET_STATUS: return CMD_INTENT_GET_STATUS;
    case TINYML_INTENT_LIST_DEVICES: return CMD_INTENT_LIST_DEVICES;
    case TINYML_INTENT_HELP: return CMD_INTENT_HELP;
    case TINYML_INTENT_UNKNOWN:
    default: return CMD_INTENT_UNKNOWN;
    }
}
#endif

static bool intent_actuates(command_intent_t intent)
{
    return intent == CMD_INTENT_TURN_ON || intent == CMD_INTENT_TURN_OFF ||
           intent == CMD_INTENT_TOGGLE;
}

static const char *intent_log_name(command_intent_t intent)
{
    switch (intent) {
    case CMD_INTENT_TURN_ON: return "TURN_ON";
    case CMD_INTENT_TURN_OFF: return "TURN_OFF";
    case CMD_INTENT_TOGGLE: return "TOGGLE";
    case CMD_INTENT_GET_STATUS: return "GET_STATUS";
    case CMD_INTENT_LIST_DEVICES: return "LIST_DEVICES";
    case CMD_INTENT_HELP: return "HELP";
    default: return "UNKNOWN";
    }
}

static void log_rejection(pm_origin_t origin, const char *intent,
                          float confidence, const char *target,
                          const char *reason)
{
    pin_manager_log_event(origin, intent, confidence, -1, target,
                          "command-engine", reason);
}

static bool classify_internal(const char *text, command_result_t *result,
                              message_t *message)
{
    entity_set_t entities;
    entity_safety_t safety = entity_extract(text, &entities);
    if (safety != ENTITY_SAFE) {
        result->guard_rejected = true;
        result->rejected = true;
        result->intent = CMD_INTENT_NONE;
        result->status = CMD_REJECTED;
        message_appendf(message, "guard: %s; no action taken",
                        entity_safety_name(safety));
        return false;
    }

    if (tokens_end_with_conjunction(&entities)) {
        result->guard_rejected = true;
        result->rejected = true;
        result->intent = CMD_INTENT_NONE;
        result->status = CMD_REJECTED;
        message_appendf(message, "guard: incomplete conjunction; no action taken");
        return false;
    }

#if defined(CMD_AI_COMPILED)
    if (s_model == NULL) {
        result->status = CMD_AI_DISABLED;
        message_appendf(message, "model unavailable; no action taken");
        return false;
    }

    tinyml_tokens_t tokens;
    tinyml_text_status_t text_status = tinyml_tokenize(entities.text, &tokens);
    if (text_status != TINYML_TEXT_OK) {
        result->status = CMD_REJECTED;
        result->rejected = true;
        message_appendf(message, "text: %s; no action taken",
                        tinyml_text_status_name(text_status));
        return false;
    }

    tinyml_result_t prediction;
    if (!tinyml_predict_tokens(s_model, &tokens, &prediction)) {
        result->status = CMD_REJECTED;
        result->rejected = true;
        message_appendf(message, "model: prediction failed; no action taken");
        return false;
    }

    result->intent = map_intent(prediction.intent);
    result->confidence = prediction.confidence;
    result->margin = prediction.margin;
    result->threshold = intent_actuates(result->intent)
                            ? CMD_ACTUATION_CONFIDENCE
                            : s_model->confidence_threshold;

    if (prediction.rejected) {
        result->rejected = true;
        result->status = CMD_REJECTED;
        message_appendf(message, "model: %s; no action taken",
                        tinyml_reject_reason_name(prediction.reject_reason));
        return false;
    }
    if (prediction.intent == TINYML_INTENT_UNKNOWN ||
        result->intent == CMD_INTENT_UNKNOWN) {
        result->rejected = true;
        result->status = CMD_REJECTED;
        message_appendf(message, "model: unknown intent; no action taken");
        return false;
    }
    if (intent_actuates(result->intent) &&
        (prediction.confidence < CMD_ACTUATION_CONFIDENCE ||
         prediction.margin < CMD_ACTUATION_MARGIN)) {
        result->rejected = true;
        result->status = CMD_REJECTED;
        message_appendf(message,
                        "policy: actuation needs confidence >= %.2f and margin "
                        ">= %.2f (got %.3f / %.3f); no action taken",
                        (double)CMD_ACTUATION_CONFIDENCE,
                        (double)CMD_ACTUATION_MARGIN,
                        (double)prediction.confidence,
                        (double)prediction.margin);
        return false;
    }

    result->status = CMD_OK;
    return true;
#else
    (void)message;
    result->status = CMD_AI_DISABLED;
    return false;
#endif
}

static void append_pin_status(message_t *message, const pm_pin_status_t *status)
{
    if (!status->initialized) {
        message_appendf(message, "GPIO%u %s: not initialized%s%s\n",
                        (unsigned)status->pin,
                        status->device_name[0] != '\0' ? status->device_name : "-",
                        status->reserved ? " (reserved)" : "",
                        status->configured ? "" : " (unnamed)");
        return;
    }
    message_appendf(message,
                    "GPIO%u %s: %s, %s, pull=%s, AI=%s%s%s\n",
                    (unsigned)status->pin,
                    status->device_name[0] != '\0' ? status->device_name : "-",
                    status->mode == GPIO_MODE_OUTPUT ? "output" : "input",
                    status->logical_state ? "ON" : "OFF",
                    GPIO_PullName(status->pull),
                    status->ai_allowed ? "yes" : "no",
                    status->locked ? ", LOCKED" : "",
                    status->configured ? "" : ", unnamed");
}

static void show_status(message_t *message, const resolved_target_t *target)
{
    if (target->kind == TARGET_PINS) {
        for (size_t i = 0; i < target->pin_count; ++i) {
            pm_pin_status_t status;
            if (pin_manager_get_status(target->pins[i], &status) != PM_OK) {
                message_appendf(message, "GPIO%u: invalid pin\n",
                                (unsigned)target->pins[i]);
                continue;
            }
            append_pin_status(message, &status);
        }
        return;
    }
    if (target->kind == TARGET_GROUP) {
        pm_group_status_t group;
        if (pin_manager_get_group(target->group, &group) != PM_OK) {
            message_appendf(message, "group %s: not found\n", target->group);
            return;
        }
        message_appendf(message, "group %s: %u output(s)\n", group.name,
                        (unsigned)group.pin_count);
        for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
            if ((group.pin_mask & (1ULL << pin)) == 0) {
                continue;
            }
            pm_pin_status_t status;
            if (pin_manager_get_status(pin, &status) == PM_OK) {
                append_pin_status(message, &status);
            }
        }
        return;
    }

    size_t shown = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
        pm_pin_status_t status;
        if (pin_manager_get_status(pin, &status) != PM_OK ||
            !status.initialized) {
            continue;
        }
        append_pin_status(message, &status);
        ++shown;
    }
    if (shown == 0) {
        message_appendf(message, "no initialized pins\n");
    }
}

static void show_devices(message_t *message)
{
    size_t shown = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
        pm_pin_status_t status;
        if (pin_manager_get_status(pin, &status) != PM_OK ||
            !status.configured) {
            continue;
        }
        message_appendf(message, "GPIO%u %s: %s, %s, AI=%s%s\n",
                        (unsigned)pin, status.device_name,
                        status.mode == GPIO_MODE_OUTPUT ? "output" : "input",
                        status.logical_state ? "ON" : "OFF",
                        status.ai_allowed ? "yes" : "no",
                        status.locked ? ", LOCKED" : "");
        ++shown;
    }
    if (shown == 0) {
        message_appendf(message, "no configured devices\n");
    }
}

static void show_help(message_t *message)
{
    message_appendf(
        message,
        "Natural commands: turn on/off/toggle <device|GPIO>; status <device|all>; "
        "list devices; help.\n"
        "Pin configuration, permissions and bulk commands are local-console only.\n"
        "Multi-output ON and TOGGLE require a local 'confirm'. Negations, "
        "questions,\n"
        "prompt injection and configuration text never actuate.\n");
}

static pm_status_t validate_output_pin(uint8_t pin, pm_origin_t origin,
                                       bool require_test_approved)
{
    pm_pin_status_t status;
    pm_status_t result = pin_manager_get_status(pin, &status);
    if (result != PM_OK) {
        return result;
    }
    if (!status.initialized) {
        return PM_ERR_UNINITIALIZED;
    }
    if (!status.configured) {
        return PM_ERR_NOT_CONFIGURED;
    }
    if (status.mode != GPIO_MODE_OUTPUT) {
        return PM_ERR_DIRECTION;
    }
    if (status.locked) {
        return PM_ERR_LOCKED;
    }
    if ((origin == PM_ORIGIN_AI || origin == PM_ORIGIN_REMOTE) &&
        !status.ai_allowed) {
        return PM_ERR_AI_DENIED;
    }
    if (require_test_approved && !status.test_approved) {
        return PM_ERR_AI_DENIED;
    }
    return PM_OK;
}

static pm_status_t validate_pins(const uint8_t *pins, size_t count,
                                 pm_origin_t origin, bool require_test_approved)
{
    for (size_t i = 0; i < count; ++i) {
        pm_status_t status =
            validate_output_pin(pins[i], origin, require_test_approved);
        if (status != PM_OK) {
            return status;
        }
    }
    return PM_OK;
}

static size_t collect_all_outputs(uint8_t *pins, size_t capacity,
                                  pm_origin_t origin, bool approved_only)
{
    size_t count = 0;
    for (uint8_t pin = 0; pin < PM_MAX_PINS && count < capacity; ++pin) {
        if (validate_output_pin(pin, origin, approved_only) == PM_OK) {
            pins[count++] = pin;
        }
    }
    return count;
}

static void pending_clear(void)
{
    memset(&s_pending, 0, sizeof(s_pending));
}

static bool pending_live(void)
{
    if (s_pending.kind == PENDING_NONE) {
        return false;
    }
    if ((uint32_t)(now_ms() - s_pending.created_ms) >= CMD_PENDING_TIMEOUT_MS) {
        pending_clear();
        return false;
    }
    return true;
}

static command_status_t begin_pins_pending(command_result_t *result,
                                            message_t *message,
                                            pending_kind_t kind,
                                            const uint8_t *pins, size_t count,
                                            pm_origin_t origin,
                                            command_intent_t intent,
                                            float confidence)
{
    if (count == 0 || count > PM_MAX_PINS) {
        message_appendf(message, "no targets\n");
        return CMD_NO_TARGET;
    }
    if (pending_live()) {
        message_appendf(message, "another confirmation is already pending\n");
        return CMD_BUSY;
    }
    pending_clear();
    s_pending.kind = kind;
    s_pending.pin_count = count;
    memcpy(s_pending.pins, pins, count);
    s_pending.origin = origin;
    s_pending.intent = intent;
    s_pending.confidence = confidence;
    s_pending.created_ms = now_ms();
    result->confirmation_pending = true;
    message_appendf(message,
                    "plan: %s %u output(s); no output changed. "
                    "Run local 'confirm' within %u seconds.\n",
                    intent_log_name(intent), (unsigned)count,
                    (unsigned)(CMD_PENDING_TIMEOUT_MS / 1000));
    return CMD_CONFIRMATION_REQUIRED;
}

static command_status_t begin_group_pending(command_result_t *result,
                                             message_t *message,
                                             pending_kind_t kind,
                                             const char *group,
                                             pm_origin_t origin,
                                             command_intent_t intent,
                                             float confidence)
{
    if (pending_live()) {
        message_appendf(message, "another confirmation is already pending\n");
        return CMD_BUSY;
    }
    pm_group_status_t status;
    if (pin_manager_get_group(group, &status) != PM_OK) {
        message_appendf(message, "group %s: unknown\n", group);
        return CMD_REJECTED;
    }
    pending_clear();
    s_pending.kind = kind;
    copy_text(s_pending.group, sizeof(s_pending.group), status.name);
    s_pending.origin = origin;
    s_pending.intent = intent;
    s_pending.confidence = confidence;
    s_pending.created_ms = now_ms();
    result->confirmation_pending = true;
    message_appendf(message,
                    "plan: %s group %s (%u output(s)); no output changed. "
                    "Run local 'confirm' within %u seconds.\n",
                    intent_log_name(intent), status.name,
                    (unsigned)status.pin_count,
                    (unsigned)(CMD_PENDING_TIMEOUT_MS / 1000));
    return CMD_CONFIRMATION_REQUIRED;
}

static command_status_t begin_all_pending(command_result_t *result,
                                           message_t *message,
                                           const uint8_t *pins, size_t count,
                                           pm_origin_t origin, float confidence)
{
    if (count == 0) {
        message_appendf(message, "no test-approved outputs are available\n");
        return CMD_REJECTED;
    }
    return begin_pins_pending(result, message, PENDING_ALL_ON, pins, count,
                              origin, CMD_INTENT_TURN_ON, confidence);
}

static command_status_t execute_off(message_t *message, const uint8_t *pins,
                                    size_t count, pm_origin_t origin,
                                    command_intent_t intent, float confidence)
{
    pm_status_t status;
    size_t affected = 0;
    if (count == 1) {
        status = pin_manager_output(pins[0], false, origin,
                                    intent_log_name(intent), confidence, "ai");
        if (status == PM_OK) {
            affected = 1;
        }
    } else {
        status = pin_manager_output_many(pins, count, false, true, origin,
                                         intent_log_name(intent), confidence, "ai",
                                         &affected);
    }
    if (status != PM_OK) {
        message_appendf(message, "execution: rejected: %s\n",
                        pin_manager_status_name(status));
        log_rejection(origin, intent_log_name(intent), confidence, "outputs",
                      pin_manager_status_name(status));
        return CMD_REJECTED;
    }
    message_appendf(message, "execution: ok: %u output(s) OFF\n",
                    (unsigned)affected);
    return CMD_OK;
}

static command_status_t execute_on(message_t *message, const uint8_t *pins,
                                   size_t count, pm_origin_t origin,
                                   command_intent_t intent, float confidence)
{
    pm_status_t status;
    size_t affected = 0;
    if (count == 1) {
        status = pin_manager_output(pins[0], true, origin,
                                    intent_log_name(intent), confidence, "ai");
        if (status == PM_OK) {
            affected = 1;
        }
    } else {
        status = pin_manager_output_many(pins, count, true, true, origin,
                                         intent_log_name(intent), confidence, "ai",
                                         &affected);
    }
    if (status != PM_OK) {
        message_appendf(message, "execution: rejected: %s\n",
                        pin_manager_status_name(status));
        log_rejection(origin, intent_log_name(intent), confidence, "outputs",
                      pin_manager_status_name(status));
        return CMD_REJECTED;
    }
    message_appendf(message, "execution: ok: %u output(s) ON\n",
                    (unsigned)affected);
    return CMD_OK;
}

static command_status_t execute_toggle(message_t *message, const uint8_t *pins,
                                       size_t count, pm_origin_t origin,
                                       command_intent_t intent, float confidence)
{
    if (count == 1) {
        pm_status_t status = pin_manager_toggle(
            pins[0], origin, intent_log_name(intent), confidence, "ai");
        if (status != PM_OK) {
            message_appendf(message, "execution: rejected: %s\n",
                            pin_manager_status_name(status));
            log_rejection(origin, intent_log_name(intent), confidence, "output",
                          pin_manager_status_name(status));
            return CMD_REJECTED;
        }
        message_appendf(message, "execution: ok: output toggled\n");
        return CMD_OK;
    }

    /* Partition by the desired state so the actual writes still use the
     * register-mask bulk API. All targets were validated above. */
    uint8_t turn_on[PM_MAX_PINS];
    uint8_t turn_off[PM_MAX_PINS];
    size_t on_count = 0;
    size_t off_count = 0;
    for (size_t i = 0; i < count; ++i) {
        pm_pin_status_t status;
        if (pin_manager_get_status(pins[i], &status) != PM_OK) {
            message_appendf(message, "execution: rejected: invalid target\n");
            return CMD_REJECTED;
        }
        if (status.logical_state) {
            turn_off[off_count++] = pins[i];
        } else {
            turn_on[on_count++] = pins[i];
        }
    }
    size_t affected = 0;
    if (off_count > 0) {
        pm_status_t status = pin_manager_output_many(
            turn_off, off_count, false, true, origin, intent_log_name(intent),
            confidence, "ai", &affected);
        if (status != PM_OK) {
            message_appendf(message, "execution: rejected: %s\n",
                            pin_manager_status_name(status));
            return CMD_REJECTED;
        }
    }
    size_t on_affected = 0;
    if (on_count > 0) {
        pm_status_t status = pin_manager_output_many(
            turn_on, on_count, true, true, origin, intent_log_name(intent),
            confidence, "ai", &on_affected);
        if (status != PM_OK) {
            message_appendf(message, "execution: rejected: %s\n",
                            pin_manager_status_name(status));
            return CMD_REJECTED;
        }
    }
    message_appendf(message,
                    "execution: ok: %u output(s) toggled (%u ON, %u OFF)\n",
                    (unsigned)(affected + on_affected), (unsigned)on_affected,
                    (unsigned)affected);
    return CMD_OK;
}

static command_status_t reject_target(command_result_t *result,
                                      message_t *message, pm_origin_t origin,
                                      command_intent_t intent, float confidence,
                                      const char *reason)
{
    result->status = CMD_REJECTED;
    result->rejected = true;
    message_appendf(message, "execution: rejected: %s; no action taken\n", reason);
    log_rejection(origin, intent_log_name(intent), confidence, "target", reason);
    return CMD_REJECTED;
}

static command_status_t execute_command(const char *text, pm_origin_t origin,
                                        command_result_t *result,
                                        message_t *message)
{
    entity_set_t entities;
    /* classify_internal has already validated this exact text; extract again to
     * get the normalized tokens without sharing mutable state. */
    if (entity_extract(text, &entities) != ENTITY_SAFE) {
        return reject_target(result, message, origin, result->intent,
                             result->confidence, "guard");
    }

    command_intent_t intent = result->intent;
    if (intent == CMD_INTENT_LIST_DEVICES) {
        show_devices(message);
        return CMD_OK;
    }
    if (intent == CMD_INTENT_HELP) {
        show_help(message);
        return CMD_OK;
    }

    resolved_target_t target;
    resolve_target(&entities, &target);

    if (intent == CMD_INTENT_GET_STATUS) {
        if (target.ambiguous) {
            return reject_target(result, message, origin, intent,
                                 result->confidence, "ambiguous target");
        }
        if (target.kind == TARGET_NONE) {
            target.kind = TARGET_ALL;
            set_target_label(&target);
        }
        show_status(message, &target);
        return CMD_OK;
    }

    if (!intent_actuates(intent)) {
        return reject_target(result, message, origin, intent, result->confidence,
                             "unsupported intent");
    }
    if (target.ambiguous) {
        return reject_target(result, message, origin, intent, result->confidence,
                             "ambiguous target");
    }
    if (target.kind == TARGET_NONE) {
        return reject_target(result, message, origin, intent, result->confidence,
                             "missing target");
    }

    if (target.kind == TARGET_GROUP) {
        uint8_t group_pins[PM_MAX_PINS];
        size_t group_count =
            group_pin_list(target.group, group_pins, PM_MAX_PINS);
        if (group_count == 0) {
            return reject_target(result, message, origin, intent,
                                 result->confidence, "empty group");
        }
        if (intent != CMD_INTENT_TURN_OFF) {
            pm_status_t validation =
                validate_pins(group_pins, group_count, origin,
                              intent == CMD_INTENT_TURN_ON);
            if (validation != PM_OK) {
                return reject_target(result, message, origin, intent,
                                     result->confidence,
                                     pin_manager_status_name(validation));
            }
        }
        if (intent == CMD_INTENT_TURN_OFF) {
            size_t affected = 0;
            pm_status_t status = pin_manager_group_apply(
                target.group, false, true, origin, "TURN_OFF",
                result->confidence, "ai", &affected);
            if (status != PM_OK) {
                message_appendf(message, "execution: rejected: %s\n",
                                pin_manager_status_name(status));
                return CMD_REJECTED;
            }
            message_appendf(message, "execution: ok: group %s OFF (%u output(s))\n",
                            target.group, (unsigned)affected);
            return CMD_OK;
        }
        if (intent == CMD_INTENT_TURN_ON) {
            return begin_group_pending(result, message, PENDING_GROUP_ON,
                                       target.group, origin, intent,
                                       result->confidence);
        }
        return begin_group_pending(result, message, PENDING_GROUP_TOGGLE,
                                   target.group, origin, intent,
                                   result->confidence);
    }

    if (target.kind == TARGET_ALL) {
        uint8_t pins[PM_MAX_PINS];
        size_t count = 0;
        if (intent == CMD_INTENT_TURN_OFF) {
            count = collect_all_outputs(pins, PM_MAX_PINS, origin, false);
            if (count == 0) {
                message_appendf(message, "execution: ok: no configured outputs\n");
                return CMD_OK;
            }
            return execute_off(message, pins, count, origin, intent,
                               result->confidence);
        }
        if (intent == CMD_INTENT_TURN_ON) {
            count = collect_all_outputs(pins, PM_MAX_PINS, origin, true);
            return begin_all_pending(result, message, pins, count, origin,
                                     result->confidence);
        }
        return reject_target(result, message, origin, intent, result->confidence,
                             "all-toggle-unsupported");
    }

    /* Explicit pins or a resolved device name. */
    if (intent == CMD_INTENT_TURN_ON) {
        pm_status_t validation =
            validate_pins(target.pins, target.pin_count, origin, false);
        if (validation != PM_OK) {
            return reject_target(result, message, origin, intent,
                                 result->confidence,
                                 pin_manager_status_name(validation));
        }
        if (target.pin_count > 1) {
            return begin_pins_pending(result, message, PENDING_PINS_ON,
                                      target.pins, target.pin_count, origin,
                                      intent, result->confidence);
        }
        return execute_on(message, target.pins, target.pin_count, origin, intent,
                          result->confidence);
    }
    if (intent == CMD_INTENT_TURN_OFF) {
        pm_status_t validation =
            validate_pins(target.pins, target.pin_count, origin, false);
        if (validation != PM_OK) {
            return reject_target(result, message, origin, intent,
                                 result->confidence,
                                 pin_manager_status_name(validation));
        }
        return execute_off(message, target.pins, target.pin_count, origin, intent,
                           result->confidence);
    }
    if (intent == CMD_INTENT_TOGGLE) {
        pm_status_t validation =
            validate_pins(target.pins, target.pin_count, origin, false);
        if (validation != PM_OK) {
            return reject_target(result, message, origin, intent,
                                 result->confidence,
                                 pin_manager_status_name(validation));
        }
        if (target.pin_count > 1) {
            return begin_pins_pending(result, message, PENDING_PINS_TOGGLE,
                                      target.pins, target.pin_count, origin,
                                      intent, result->confidence);
        }
        return execute_toggle(message, target.pins, target.pin_count, origin,
                              intent, result->confidence);
    }

    return reject_target(result, message, origin, intent, result->confidence,
                         "unsupported intent");
}

void command_engine_init(bool ai_enabled)
{
    pending_clear();
    s_initialized = true;
#if defined(CMD_AI_COMPILED)
    s_model = model_data_get();
    s_ai_enabled = ai_enabled && s_model != NULL;
#else
    (void)ai_enabled;
    s_ai_enabled = false;
#endif
}

bool command_engine_ai_available(void)
{
#if defined(CMD_AI_COMPILED)
    return s_model != NULL;
#else
    return false;
#endif
}

bool command_engine_ai_enabled(void)
{
    return command_engine_ai_available() && s_ai_enabled;
}

void command_engine_set_ai_enabled(bool enabled)
{
    s_ai_enabled = command_engine_ai_available() && enabled;
    if (!s_ai_enabled) {
        pending_clear();
    }
}

command_status_t command_engine_classify(const char *text,
                                          command_result_t *result)
{
    if (result == NULL) {
        return CMD_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->intent = CMD_INTENT_NONE;
    message_t message;
    message_init(&message, result);

    if (!s_initialized) {
        command_engine_init(false);
    }
    if (!command_engine_ai_available()) {
        result->status = CMD_AI_DISABLED;
        message_appendf(&message, "TinyAI is not compiled into this firmware");
        return result->status;
    }
    if (!s_ai_enabled) {
        result->status = CMD_AI_DISABLED;
        message_appendf(&message, "TinyAI is disabled; use 'ai on' first");
        return result->status;
    }
    if (text == NULL) {
        result->status = CMD_INVALID_ARGUMENT;
        message_appendf(&message, "missing text");
        return result->status;
    }

    if (!classify_internal(text, result, &message)) {
        log_rejection(PM_ORIGIN_AI, "CLASSIFY", result->confidence, "text",
                      result->message);
        return result->status;
    }
    message_appendf(&message, "classify only: no action taken");
    return result->status;
}

command_status_t command_engine_handle_text(const char *text,
                                             pm_origin_t origin,
                                             command_result_t *result)
{
    if (result == NULL) {
        return CMD_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->intent = CMD_INTENT_NONE;
    message_t message;
    message_init(&message, result);

    if (!s_initialized) {
        command_engine_init(false);
    }
    if (!command_engine_ai_available()) {
        result->status = CMD_AI_DISABLED;
        message_appendf(&message, "TinyAI is not compiled into this firmware");
        return result->status;
    }
    if (!s_ai_enabled) {
        result->status = CMD_AI_DISABLED;
        message_appendf(&message, "TinyAI is disabled; use 'ai on' first");
        return result->status;
    }
    if (text == NULL) {
        result->status = CMD_INVALID_ARGUMENT;
        message_appendf(&message, "missing text");
        return result->status;
    }
    if (pending_live()) {
        result->status = CMD_BUSY;
        result->confirmation_pending = true;
        message_appendf(&message,
                        "a confirmation is pending; run 'confirm' or 'cancel'");
        return result->status;
    }

    if (!classify_internal(text, result, &message)) {
        log_rejection(origin, "REJECT", result->confidence, "text",
                      result->message);
        return result->status;
    }

    command_status_t status = execute_command(text, origin, result, &message);
    result->status = status;
    return status;
}

command_status_t command_engine_confirm(command_result_t *result)
{
    if (result == NULL) {
        return CMD_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->intent = CMD_INTENT_NONE;
    message_t message;
    message_init(&message, result);

    if (!pending_live()) {
        result->status = CMD_REJECTED;
        message_appendf(&message, "no pending confirmation (or it expired)");
        return result->status;
    }

    pending_plan_t plan = s_pending;
    pending_clear();
    result->intent = plan.intent;
    result->confidence = plan.confidence;

    const char *intent_name = intent_log_name(plan.intent);
    pm_status_t status = PM_OK;
    size_t affected = 0;

    switch (plan.kind) {
    case PENDING_PINS_ON:
    case PENDING_ALL_ON:
        status = validate_pins(plan.pins, plan.pin_count, plan.origin,
                               plan.kind == PENDING_ALL_ON);
        if (status == PM_OK) {
            status = pin_manager_output_many(
                plan.pins, plan.pin_count, true, true, plan.origin,
                intent_name, plan.confidence, "console-confirm", &affected);
        }
        break;
    case PENDING_GROUP_ON:
        status = pin_manager_group_apply(
            plan.group, true, true, plan.origin, intent_name, plan.confidence,
            "console-confirm", &affected);
        break;
    case PENDING_PINS_TOGGLE:
    case PENDING_GROUP_TOGGLE: {
        uint8_t pins[PM_MAX_PINS];
        size_t count = 0;
        if (plan.kind == PENDING_PINS_TOGGLE) {
            memcpy(pins, plan.pins, plan.pin_count);
            count = plan.pin_count;
        } else {
            count = group_pin_list(plan.group, pins, PM_MAX_PINS);
        }
        status = validate_pins(pins, count, plan.origin, false);
        if (status == PM_OK) {
            uint8_t turn_on[PM_MAX_PINS];
            uint8_t turn_off[PM_MAX_PINS];
            size_t on_count = 0;
            size_t off_count = 0;
            for (size_t i = 0; i < count; ++i) {
                pm_pin_status_t pin_status;
                if (pin_manager_get_status(pins[i], &pin_status) != PM_OK) {
                    status = PM_ERR_INVALID_PIN;
                    break;
                }
                if (pin_status.logical_state) {
                    turn_off[off_count++] = pins[i];
                } else {
                    turn_on[on_count++] = pins[i];
                }
            }
            if (status == PM_OK && off_count > 0) {
                status = pin_manager_output_many(
                    turn_off, off_count, false, true, plan.origin, intent_name,
                    plan.confidence, "console-confirm", &affected);
            }
            if (status == PM_OK && on_count > 0) {
                size_t on_affected = 0;
                status = pin_manager_output_many(
                    turn_on, on_count, true, true, plan.origin, intent_name,
                    plan.confidence, "console-confirm", &on_affected);
                affected += on_affected;
            }
        }
        break;
    }
    case PENDING_NONE:
    default:
        status = PM_ERR_NOT_IMPLEMENTED;
        break;
    }

    if (status != PM_OK) {
        result->status = CMD_REJECTED;
        result->rejected = true;
        message_appendf(&message, "execution: rejected: %s\n",
                        pin_manager_status_name(status));
        log_rejection(plan.origin, intent_name, plan.confidence, "confirm",
                      pin_manager_status_name(status));
        return result->status;
    }

    char target[PM_GROUP_NAME_MAX + 8];
    if (plan.kind == PENDING_GROUP_ON || plan.kind == PENDING_GROUP_TOGGLE) {
        snprintf(target, sizeof(target), "group:%s", plan.group);
    } else {
        snprintf(target, sizeof(target), "confirmed-plan");
    }
    pin_manager_log_event(plan.origin, intent_name, plan.confidence, -1, target,
                          "console-confirm", "confirmed");
    result->status = CMD_OK;
    message_appendf(&message, "execution: confirmed: %u output(s) affected\n",
                    (unsigned)affected);
    return result->status;
}

void command_engine_cancel(void)
{
    pending_clear();
}

bool command_engine_confirmation_pending(void)
{
    return pending_live();
}

bool command_engine_model_available(void)
{
    return command_engine_ai_available();
}

bool command_engine_model_info(char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return false;
    }
#if defined(CMD_AI_COMPILED)
    const tinyml_model_t *model = s_model != NULL ? s_model : model_data_get();
    if (model == NULL) {
        snprintf(out, capacity, "model: unavailable");
        return false;
    }
    snprintf(out, capacity,
             "model: %s\nbytes=%u classes=%u buckets=%u ngram=%u..%u\n"
             "limits: text<=%u bytes tokens<=%u confidence>=%.2f margin>=%.2f\n"
             "dataset_hash=0x%08x labels=",
             model->version, (unsigned)model->model_bytes,
             (unsigned)model->classes, (unsigned)model->feature_buckets,
             (unsigned)model->char_ngram_min, (unsigned)model->char_ngram_max,
             (unsigned)model->max_text_bytes, (unsigned)model->max_tokens,
             (double)model->confidence_threshold,
             (double)model->margin_threshold, (unsigned)model->dataset_hash);
    size_t used = strlen(out);
    for (uint8_t i = 0; i < model->classes && used + 1 < capacity; ++i) {
        int written = snprintf(out + used, capacity - used, "%s%s",
                               i == 0 ? "" : ",", model->labels[i]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
        if (used >= capacity) {
            out[capacity - 1] = '\0';
            break;
        }
    }
    return true;
#else
    snprintf(out, capacity,
             "model: unavailable (TinyAI disabled at build time)");
    return false;
#endif
}

bool command_engine_benchmark(unsigned iterations, uint32_t *elapsed_us,
                              uint32_t *predictions)
{
    if (elapsed_us != NULL) {
        *elapsed_us = 0;
    }
    if (predictions != NULL) {
        *predictions = 0;
    }
#if defined(CMD_AI_COMPILED)
    if (iterations == 0 || s_model == NULL) {
        return false;
    }
    const char *text = "turn on the workshop light";
    tinyml_result_t result;
#ifdef ESP_PLATFORM
    int64_t start = esp_timer_get_time();
#else
    clock_t start = clock();
#endif
    uint32_t completed = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        if (!tinyml_predict_text(s_model, text, &result)) {
            break;
        }
        ++completed;
    }
#ifdef ESP_PLATFORM
    int64_t end = esp_timer_get_time();
    uint64_t elapsed = (uint64_t)(end - start);
#else
    clock_t end = clock();
    uint64_t elapsed =
        (uint64_t)((double)(end - start) * 1000000.0 / (double)CLOCKS_PER_SEC);
#endif
    if (completed == 0) {
        return false;
    }
    if (elapsed == 0) {
        elapsed = 1;
    }
    if (elapsed_us != NULL) {
        *elapsed_us = (uint32_t)elapsed;
    }
    if (predictions != NULL) {
        *predictions = completed;
    }
    return true;
#else
    (void)iterations;
    return false;
#endif
}

const char *command_status_name(command_status_t status)
{
    switch (status) {
    case CMD_OK: return "ok";
    case CMD_REJECTED: return "rejected";
    case CMD_CONFIRMATION_REQUIRED: return "confirmation-required";
    case CMD_AI_DISABLED: return "ai-disabled";
    case CMD_NO_TARGET: return "no-target";
    case CMD_AMBIGUOUS: return "ambiguous";
    case CMD_INVALID_ARGUMENT: return "invalid-argument";
    case CMD_BUSY: return "busy";
    default: return "unknown";
    }
}

const char *command_intent_name(command_intent_t intent)
{
    switch (intent) {
    case CMD_INTENT_TURN_ON: return "TURN_ON";
    case CMD_INTENT_TURN_OFF: return "TURN_OFF";
    case CMD_INTENT_TOGGLE: return "TOGGLE";
    case CMD_INTENT_GET_STATUS: return "GET_STATUS";
    case CMD_INTENT_LIST_DEVICES: return "LIST_DEVICES";
    case CMD_INTENT_HELP: return "HELP";
    case CMD_INTENT_UNKNOWN: return "UNKNOWN";
    case CMD_INTENT_NONE:
    default: return "NONE";
    }
}

#ifdef COMMAND_ENGINE_HOST_TEST
void command_engine_test_set_time(uint32_t now_ms)
{
    s_host_time_ms = now_ms;
}
#endif
