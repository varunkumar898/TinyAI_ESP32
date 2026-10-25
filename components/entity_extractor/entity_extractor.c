#include "entity_extractor.h"

#include <ctype.h>
#include <string.h>

static bool is_ascii_alnum(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

static entity_safety_t normalize_text(const char *text, char *out, size_t capacity)
{
    if (text == NULL || out == NULL || capacity == 0) {
        return ENTITY_REJECT_EMPTY;
    }
    size_t length = strlen(text);
    if (length > ENTITY_TEXT_MAX) {
        return ENTITY_REJECT_TOO_LONG;
    }

    size_t used = 0;
    bool previous_space = true;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x80) {
            out[0] = '\0';
            return ENTITY_REJECT_NON_ASCII;
        }
        if (is_ascii_alnum(c)) {
            if (used + 1 >= capacity) {
                out[0] = '\0';
                return ENTITY_REJECT_TOO_LONG;
            }
            out[used++] = (char)tolower(c);
            previous_space = false;
        } else if (c == '\'') {
            /* don't -> dont, matching the TinyML tokenizer. */
            continue;
        } else if (!previous_space && used > 0) {
            if (used + 1 >= capacity) {
                out[0] = '\0';
                return ENTITY_REJECT_TOO_LONG;
            }
            out[used++] = ' ';
            previous_space = true;
        }
    }
    if (used > 0 && out[used - 1] == ' ') {
        --used;
    }
    out[used] = '\0';
    return used == 0 ? ENTITY_REJECT_EMPTY : ENTITY_SAFE;
}

static entity_safety_t tokenize_normalized(const char *normalized, entity_set_t *out)
{
    size_t length = strlen(normalized);
    char token[ENTITY_MAX_TOKEN_CHARS + 8];
    size_t token_length = 0;
    bool too_long = false;
    uint8_t count = 0;

    for (size_t i = 0; i <= length; ++i) {
        char c = (i == length) ? ' ' : normalized[i];
        if (c == ' ') {
            if (token_length == 0) {
                continue;
            }
            if (too_long) {
                return ENTITY_REJECT_TOKEN_TOO_LONG;
            }
            if (count >= ENTITY_MAX_TOKENS) {
                return ENTITY_REJECT_TOO_MANY_TOKENS;
            }
            memcpy(out->tokens[count], token, token_length);
            out->tokens[count][token_length] = '\0';
            ++count;
            token_length = 0;
            too_long = false;
            continue;
        }
        if (token_length < sizeof(token)) {
            token[token_length++] = c;
        }
        if (token_length > ENTITY_MAX_TOKEN_CHARS) {
            too_long = true;
        }
    }
    out->token_count = count;
    return count == 0 ? ENTITY_REJECT_EMPTY : ENTITY_SAFE;
}

bool entity_has_token(const entity_set_t *set, const char *token)
{
    if (set == NULL || token == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < set->token_count; ++i) {
        if (strcmp(set->tokens[i], token) == 0) {
            return true;
        }
    }
    return false;
}

bool entity_has_sequence(const entity_set_t *set, uint8_t start,
                         const char *const *tokens, size_t count)
{
    if (set == NULL || tokens == NULL || count == 0) {
        return false;
    }
    if ((size_t)start + count > set->token_count) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(set->tokens[start + i], tokens[i]) != 0) {
            return false;
        }
    }
    return true;
}

static bool has_phrase(const entity_set_t *set, const char *const *tokens, size_t count)
{
    if ((size_t)count > set->token_count) {
        return false;
    }
    for (uint8_t start = 0; start + count <= set->token_count; ++start) {
        if (entity_has_sequence(set, start, tokens, count)) {
            return true;
        }
    }
    return false;
}

static bool has_any_token(const entity_set_t *set, const char *const *tokens, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (entity_has_token(set, tokens[i])) {
            return true;
        }
    }
    return false;
}

static bool is_pin_marker(const char *token)
{
    return strcmp(token, "pin") == 0 || strcmp(token, "gpio") == 0;
}

static void add_pin(entity_set_t *set, uint16_t pin)
{
    if (pin > 255) {
        return;
    }
    for (uint8_t i = 0; i < set->explicit_pin_count; ++i) {
        if (set->explicit_pins[i] == (uint8_t)pin) {
            return;
        }
    }
    if (set->explicit_pin_count < ENTITY_MAX_PINS) {
        set->explicit_pins[set->explicit_pin_count++] = (uint8_t)pin;
    }
}

static int digit_value(const char *token)
{
    if (token[0] < '0' || token[0] > '9' || token[1] != '\0') {
        return -1;
    }
    return token[0] - '0';
}

static int word_value(const char *token)
{
    static const char *const words[] = {
        "zero", "one", "two", "three", "four", "five", "six", "seven",
        "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
        "fifteen", "sixteen", "seventeen", "eighteen", "nineteen",
    };
    for (int i = 0; i < 20; ++i) {
        if (strcmp(token, words[i]) == 0) {
            return i;
        }
    }
    if (strcmp(token, "twenty") == 0) {
        return 20;
    }
    if (strcmp(token, "thirty") == 0) {
        return 30;
    }
    return -1;
}

/* Supports digits ("23"), conventional number words ("twenty five"),
 * tens+units ("twenty-five"), and digit-by-digit speech ("two seven" = 27). */
static bool parse_number(const entity_set_t *set, uint8_t index, uint16_t *value,
                         uint8_t *consumed)
{
    if (index >= set->token_count) {
        return false;
    }
    const char *first = set->tokens[index];
    if (first[0] >= '0' && first[0] <= '9') {
        uint32_t parsed = 0;
        for (const char *p = first; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') {
                return false;
            }
            parsed = parsed * 10u + (uint32_t)(*p - '0');
            if (parsed > 255u) {
                return false;
            }
        }
        *value = (uint16_t)parsed;
        *consumed = 1;
        return true;
    }

    int word = word_value(first);
    if (word < 0) {
        return false;
    }
    *value = (uint16_t)word;
    *consumed = 1;

    if (word >= 20 && index + 1 < set->token_count) {
        int unit = word_value(set->tokens[index + 1]);
        if (unit >= 1 && unit <= 9) {
            *value = (uint16_t)(word + unit);
            *consumed = 2;
        }
    } else if (word >= 0 && word <= 9 && index + 1 < set->token_count) {
        int next = digit_value(set->tokens[index + 1]);
        if (next < 0) {
            next = word_value(set->tokens[index + 1]);
        }
        if (next >= 0 && next <= 9) {
            *value = (uint16_t)(word * 10 + next);
            *consumed = 2;
        }
    }
    return true;
}

static void extract_pins(entity_set_t *set)
{
    for (uint8_t i = 0; i < set->token_count; ++i) {
        const char *token = set->tokens[i];

        /* GPIO23 / pin25 */
        if ((strncmp(token, "gpio", 4) == 0 || strncmp(token, "pin", 3) == 0)) {
            const char *suffix = (token[0] == 'g') ? token + 4 : token + 3;
            if (*suffix != '\0') {
                bool digits = true;
                for (const char *p = suffix; *p != '\0'; ++p) {
                    if (*p < '0' || *p > '9') {
                        digits = false;
                        break;
                    }
                }
                if (digits) {
                    uint16_t pin = 0;
                    for (const char *p = suffix; *p != '\0'; ++p) {
                        pin = (uint16_t)(pin * 10u + (uint16_t)(*p - '0'));
                        if (pin > 255) {
                            break;
                        }
                    }
                    add_pin(set, pin);
                }
            }
        }

        if (!is_pin_marker(token) || i + 1 >= set->token_count) {
            continue;
        }

        uint8_t cursor = (uint8_t)(i + 1);
        uint16_t pin = 0;
        uint8_t consumed = 0;
        if (!parse_number(set, cursor, &pin, &consumed)) {
            continue;
        }
        add_pin(set, pin);
        cursor = (uint8_t)(cursor + consumed);

        /* "pin 18 and 25" / "pin 18, 25" (comma becomes whitespace). */
        while (cursor < set->token_count) {
            if (strcmp(set->tokens[cursor], "and") == 0 ||
                strcmp(set->tokens[cursor], "or") == 0) {
                ++cursor;
            }
            if (!parse_number(set, cursor, &pin, &consumed)) {
                break;
            }
            add_pin(set, pin);
            cursor = (uint8_t)(cursor + consumed);
        }
        i = (uint8_t)(cursor - 1);
    }
}

static bool detect_injection(const entity_set_t *set)
{
    static const char *const reject_tokens[] = {
        "ignore", "disregard", "override", "bypass", "jailbreak", "sudo",
        "system", "prompt", "unfiltered", "dan",
    };
    if (has_any_token(set, reject_tokens,
                      sizeof(reject_tokens) / sizeof(reject_tokens[0]))) {
        return true;
    }

    static const char *const phrases[][4] = {
        {"forget", "your", "rules", NULL},
        {"forget", "the", "rules", NULL},
        {"forget", "all", "rules", NULL},
        {"admin", "mode", NULL, NULL},
        {"you", "are", "now", NULL},
        {"act", "as", "root", NULL},
        {"repeat", "after", "me", NULL},
        {"you", "have", "permission", NULL},
        {"developer", "mode", NULL, NULL},
        {"role", "play", NULL, NULL},
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
        size_t count = 0;
        while (count < 4 && phrases[i][count] != NULL) {
            ++count;
        }
        if (has_phrase(set, phrases[i], count)) {
            return true;
        }
    }
    return false;
}

static bool detect_negation(const entity_set_t *set)
{
    static const char *const tokens[] = {
        "not", "dont", "never", "avoid", "without", "no", "cannot", "cant",
        "shouldnt", "wouldnt", "mustnt", "nor", "neither",
    };
    return has_any_token(set, tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static bool detect_hypothetical(const entity_set_t *set)
{
    static const char *const tokens[] = {
        "if", "maybe", "perhaps", "suppose", "supposing", "imagine",
        "hypothetically", "simulate", "simulated", "pretend", "whether",
    };
    if (has_any_token(set, tokens, sizeof(tokens) / sizeof(tokens[0]))) {
        return true;
    }

    static const char *const phrases[][2] = {
        {"what", "would"}, {"what", "if"}, {"what", "happens"},
        {"would", "it"}, {"would", "that"}, {"should", "i"},
        {"should", "we"}, {"could", "turning"}, {"will", "the"},
        {"when", "should"}, {"why", "is"}, {"how", "would"},
        {"is", "it"}, {"is", "that"}, {"can", "turning"},
        {"could", "it"}, {"should", "the"},
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
        if (has_phrase(set, phrases[i], 2)) {
            return true;
        }
    }
    return false;
}

static bool detect_configuration(const entity_set_t *set)
{
    static const char *const tokens[] = {
        "configure", "config", "configuration", "setup", "initialize",
        "initialise", "init", "assign", "reconfigure",
    };
    if (has_any_token(set, tokens, sizeof(tokens) / sizeof(tokens[0]))) {
        return true;
    }

    static const char *const phrases[][3] = {
        {"output", "mode", NULL},
        {"input", "mode", NULL},
        {"as", "output", NULL},
        {"as", "input", NULL},
        {"to", "output", NULL},
        {"to", "input", NULL},
        {"into", "output", NULL},
        {"into", "input", NULL},
        {"an", "output", NULL},
        {"an", "input", NULL},
    };
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
        size_t count = 0;
        while (count < 3 && phrases[i][count] != NULL) {
            ++count;
        }
        if (has_phrase(set, phrases[i], count)) {
            return true;
        }
    }

    bool make = entity_has_token(set, "make") || entity_has_token(set, "set") ||
                entity_has_token(set, "change") || entity_has_token(set, "put");
    if (make && (entity_has_token(set, "output") || entity_has_token(set, "input"))) {
        return true;
    }
    return false;
}

entity_safety_t entity_extract(const char *text, entity_set_t *out)
{
    if (out == NULL) {
        return ENTITY_REJECT_EMPTY;
    }
    memset(out, 0, sizeof(*out));

    entity_safety_t safety = normalize_text(text, out->text, sizeof(out->text));
    if (safety != ENTITY_SAFE) {
        out->safety = safety;
        return safety;
    }
    safety = tokenize_normalized(out->text, out);
    if (safety != ENTITY_SAFE) {
        out->safety = safety;
        return safety;
    }

    extract_pins(out);

    out->mentions_all = entity_has_token(out, "all") || entity_has_token(out, "every") ||
                        entity_has_token(out, "everything") ||
                        entity_has_token(out, "everyone") || entity_has_token(out, "each");
    out->mentions_any = entity_has_token(out, "any") || entity_has_token(out, "anything");
    out->mentions_confirm = entity_has_token(out, "confirm") ||
                            entity_has_token(out, "confirmed");

    if (detect_injection(out)) {
        safety = ENTITY_REJECT_INJECTION;
    } else if (detect_negation(out)) {
        safety = ENTITY_REJECT_NEGATION;
    } else if (detect_hypothetical(out)) {
        safety = ENTITY_REJECT_HYPOTHETICAL;
    } else if (detect_configuration(out)) {
        safety = ENTITY_REJECT_CONFIGURATION;
    }
    out->safety = safety;
    return safety;
}

bool entity_has_pin(const entity_set_t *set, uint8_t pin)
{
    if (set == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < set->explicit_pin_count; ++i) {
        if (set->explicit_pins[i] == pin) {
            return true;
        }
    }
    return false;
}

const char *entity_safety_name(entity_safety_t safety)
{
    switch (safety) {
    case ENTITY_SAFE: return "safe";
    case ENTITY_REJECT_EMPTY: return "empty";
    case ENTITY_REJECT_TOO_LONG: return "too-long";
    case ENTITY_REJECT_NON_ASCII: return "non-ascii";
    case ENTITY_REJECT_TOO_MANY_TOKENS: return "too-many-tokens";
    case ENTITY_REJECT_TOKEN_TOO_LONG: return "token-too-long";
    case ENTITY_REJECT_NEGATION: return "negation";
    case ENTITY_REJECT_HYPOTHETICAL: return "hypothetical-or-question";
    case ENTITY_REJECT_INJECTION: return "prompt-injection-or-roleplay";
    case ENTITY_REJECT_CONFIGURATION: return "configuration-is-local-only";
    default: return "unknown";
    }
}
