/*
 * Portable quantized intent classifier.
 *
 * This file uses no heap, no stdio and no ESP-IDF APIs. The tokenizer and
 * feature hashing are mirrored exactly by tools/tinyai_features.py. Inference
 * is integer accumulation followed by one float softmax.
 */

#include "tinyml.h"

#include <math.h>
#include <string.h>

#define FNV_OFFSET_BASIS 2166136261u
#define FNV_PRIME 16777619u

static bool ascii_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

tinyml_text_status_t tinyml_tokenize(const char *text, tinyml_tokens_t *out)
{
    if (text == NULL || out == NULL) {
        return TINYML_TEXT_INVALID_ARGUMENT;
    }

    memset(out, 0, sizeof(*out));

    size_t length = strlen(text);
    if (length > TINYML_MAX_TEXT_BYTES) {
        return TINYML_TEXT_TOO_LONG;
    }
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)text[i] >= 0x80) {
            return TINYML_TEXT_NON_ASCII;
        }
    }

    uint8_t token_count = 0;
    uint8_t token_length = 0;
    bool token_too_long = false;

    for (size_t i = 0; i <= length; ++i) {
        char c = (i == length) ? ' ' : text[i];
        if (c == '\'') {
            /* Deleted, not treated as a separator: don't -> dont. */
            continue;
        }
        if (ascii_alnum(c)) {
            if (token_length < TINYML_MAX_TOKEN_CHARS) {
                if (token_count < TINYML_MAX_TOKENS) {
                    out->tokens[token_count][token_length] = ascii_lower(c);
                }
                ++token_length;
            } else {
                token_too_long = true;
            }
            continue;
        }

        if (token_length > 0) {
            if (token_count >= TINYML_MAX_TOKENS) {
                return TINYML_TEXT_TOO_MANY_TOKENS;
            }
            if (token_too_long) {
                return TINYML_TEXT_TOKEN_TOO_LONG;
            }
            out->tokens[token_count][token_length] = '\0';
            ++token_count;
            token_length = 0;
        }
    }

    out->count = token_count;
    if (token_count == 0) {
        return TINYML_TEXT_EMPTY;
    }
    return TINYML_TEXT_OK;
}

uint32_t tinyml_fnv1a32(const char *data)
{
    uint32_t value = FNV_OFFSET_BASIS;
    if (data == NULL) {
        return value;
    }
    for (const unsigned char *p = (const unsigned char *)data; *p != '\0'; ++p) {
        value ^= *p;
        value *= FNV_PRIME;
    }
    return value;
}

static uint16_t bucket_for(const tinyml_model_t *model, const char *key)
{
    return (uint16_t)(tinyml_fnv1a32(key) % model->feature_buckets);
}

static void append_text(char *dst, size_t capacity, size_t *used, const char *src)
{
    while (*src != '\0' && *used + 1 < capacity) {
        dst[*used] = *src;
        ++(*used);
        ++src;
    }
    dst[*used] = '\0';
}

static void append_char(char *dst, size_t capacity, size_t *used, char c)
{
    if (*used + 1 < capacity) {
        dst[*used] = c;
        ++(*used);
        dst[*used] = '\0';
    }
}

static void emit_key(const tinyml_model_t *model, const char *key,
                     int32_t logits[TINYML_MAX_LABELS], uint16_t *feature_count,
                     uint8_t *seen)
{
    uint16_t bucket = bucket_for(model, key);
    if (seen != NULL) {
        uint16_t byte = bucket >> 3;
        uint8_t bit = (uint8_t)(1u << (bucket & 7u));
        if ((seen[byte] & bit) == 0) {
            seen[byte] |= bit;
            if (*feature_count < UINT16_MAX) {
                ++(*feature_count);
            }
        }
    } else if (*feature_count < UINT16_MAX) {
        ++(*feature_count);
    }
    for (uint8_t c = 0; c < model->classes; ++c) {
        logits[c] += model->weights[(size_t)c * model->feature_buckets + bucket];
    }
}

static bool is_target_token(const char *token)
{
    static const char *const target_words[] = {
        "led", "leds", "light", "lights", "lamp", "lamps", "bulb", "bulbs",
        "fan", "fans", "relay", "relays", "aux", "auxiliary", "pin", "pins",
        "gpio", "wire", "wires", "channel", "channels", "sensor", "sensors",
        "button", "buttons", "motion", "pir",
    };
    static const char *const number_words[] = {
        "zero", "one", "two", "three", "four", "five", "six", "seven", "eight",
        "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen",
        "sixteen", "seventeen", "eighteen", "nineteen", "twenty", "thirty",
    };
    for (size_t i = 0; i < sizeof(target_words) / sizeof(target_words[0]); ++i) {
        if (strcmp(token, target_words[i]) == 0) {
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(number_words) / sizeof(number_words[0]); ++i) {
        if (strcmp(token, number_words[i]) == 0) {
            return true;
        }
    }
    bool all_digits = true;
    for (const char *p = token; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            all_digits = false;
            break;
        }
    }
    if (all_digits && *token != '\0') {
        return true;
    }
    const char *prefixes[] = {"led", "gpio", "pin"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        size_t len = strlen(prefixes[i]);
        if (strncmp(token, prefixes[i], len) == 0 && token[len] != '\0') {
            const char *p = token + len;
            while (*p >= '0' && *p <= '9') {
                ++p;
            }
            if (*p == '\0') {
                return true;
            }
        }
    }
    return false;
}

static void make_intent_tokens(const tinyml_tokens_t *tokens, tinyml_tokens_t *masked)
{
    memset(masked, 0, sizeof(*masked));
    masked->count = tokens->count;
    for (uint8_t i = 0; i < tokens->count; ++i) {
        if (is_target_token(tokens->tokens[i])) {
            strcpy(masked->tokens[i], "tg");
        } else {
            strncpy(masked->tokens[i], tokens->tokens[i], TINYML_MAX_TOKEN_CHARS);
            masked->tokens[i][TINYML_MAX_TOKEN_CHARS] = '\0';
        }
    }
}

static void accumulate_all(const tinyml_model_t *model,
                           const tinyml_tokens_t *tokens,
                           int32_t logits[TINYML_MAX_LABELS],
                           uint16_t *feature_count,
                           uint8_t *seen,
                           tinyml_feature_t *out,
                           size_t capacity,
                           size_t *stored)
{
    char key[2 + TINYML_MAX_TOKEN_CHARS + 1 + TINYML_MAX_TOKEN_CHARS + 1];

    for (uint8_t t = 0; t < tokens->count; ++t) {
        key[0] = 'w';
        key[1] = ':';
        size_t used = 2;
        key[used] = '\0';
        append_text(key, sizeof(key), &used, tokens->tokens[t]);
        uint16_t bucket = bucket_for(model, key);
        emit_key(model, key, logits, feature_count, seen);
        if (out != NULL && *stored < capacity) {
            out[*stored].bucket = bucket;
            out[*stored].count = 1;
            ++(*stored);
        }
    }

    for (uint8_t t = 0; t + 1 < tokens->count; ++t) {
        key[0] = 'b';
        key[1] = ':';
        size_t used = 2;
        key[used] = '\0';
        append_text(key, sizeof(key), &used, tokens->tokens[t]);
        append_char(key, sizeof(key), &used, ' ');
        append_text(key, sizeof(key), &used, tokens->tokens[t + 1]);
        uint16_t bucket = bucket_for(model, key);
        emit_key(model, key, logits, feature_count, seen);
        if (out != NULL && *stored < capacity) {
            out[*stored].bucket = bucket;
            out[*stored].count = 1;
            ++(*stored);
        }
    }

    for (uint8_t t = 0; t < tokens->count; ++t) {
        char padded[TINYML_MAX_TOKEN_CHARS + 3];
        size_t token_len = strlen(tokens->tokens[t]);
        padded[0] = '^';
        memcpy(&padded[1], tokens->tokens[t], token_len);
        padded[token_len + 1] = '$';
        size_t padded_len = token_len + 2;

        for (uint8_t n = model->char_ngram_min; n <= model->char_ngram_max; ++n) {
            if (n < 2 || n > TINYML_MAX_TOKEN_CHARS + 2 || padded_len < n) {
                continue;
            }
            for (size_t start = 0; start + n <= padded_len; ++start) {
                key[0] = 'c';
                key[1] = (char)('0' + n);
                key[2] = ':';
                size_t used = 3;
                key[used] = '\0';
                for (size_t i = 0; i < n; ++i) {
                    append_char(key, sizeof(key), &used, padded[start + i]);
                }
                uint16_t bucket = bucket_for(model, key);
                emit_key(model, key, logits, feature_count, seen);
                if (out != NULL && *stored < capacity) {
                    out[*stored].bucket = bucket;
                    out[*stored].count = 1;
                    ++(*stored);
                }
            }
        }
    }
}

static bool model_is_valid(const tinyml_model_t *model)
{
    return model != NULL &&
           model->weights != NULL &&
           model->biases != NULL &&
           model->labels != NULL &&
           model->version != NULL &&
           model->feature_buckets > 0 &&
           model->classes > 0 &&
           model->classes <= TINYML_MAX_LABELS &&
           model->max_tokens > 0 &&
           model->max_tokens <= TINYML_MAX_TOKENS &&
           model->max_text_bytes > 0 &&
           model->max_text_bytes <= TINYML_MAX_TEXT_BYTES &&
           model->char_ngram_min >= 2 &&
           model->char_ngram_max >= model->char_ngram_min &&
           model->char_ngram_max <= TINYML_MAX_TOKEN_CHARS + 2 &&
           model->weight_scale > 0.0f;
}

bool tinyml_predict_tokens(const tinyml_model_t *model,
                           const tinyml_tokens_t *tokens,
                           tinyml_result_t *out)
{
    if (!model_is_valid(model) || tokens == NULL || out == NULL) {
        return false;
    }
    if (tokens->count == 0 || tokens->count > model->max_tokens) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    tinyml_tokens_t masked;
    make_intent_tokens(tokens, &masked);

    int32_t logits[TINYML_MAX_LABELS] = {0};
    for (uint8_t c = 0; c < model->classes; ++c) {
        logits[c] = model->biases[c];
    }
    uint16_t feature_count = 0;
    uint8_t seen[(TINYML_MAX_FEATURES + 7u) / 8u] = {0};
    accumulate_all(model, &masked, logits, &feature_count, seen, NULL, 0, &(size_t){0});

    int top = 0;
    int second = -1;
    for (uint8_t c = 0; c < model->classes; ++c) {
        out->logits[c] = logits[c];
        if (logits[c] > logits[top]) {
            second = top;
            top = c;
        } else if (c != top && (second < 0 || logits[c] > logits[second])) {
            second = c;
        }
    }

    float scaled[TINYML_MAX_LABELS];
    float max_scaled = -INFINITY;
    for (uint8_t c = 0; c < model->classes; ++c) {
        scaled[c] = (float)logits[c] * model->weight_scale;
        if (scaled[c] > max_scaled) {
            max_scaled = scaled[c];
        }
    }
    float sum = 0.0f;
    for (uint8_t c = 0; c < model->classes; ++c) {
        scaled[c] = expf(scaled[c] - max_scaled);
        sum += scaled[c];
    }
    for (uint8_t c = 0; c < model->classes; ++c) {
        scaled[c] /= sum;
    }

    out->intent = (tinyml_intent_t)top;
    out->label = model->labels[top];
    out->confidence = scaled[top];
    out->margin = (second >= 0) ? scaled[top] - scaled[second] : scaled[top];
    out->feature_count = feature_count;
    out->rejected = false;
    out->reject_reason = TINYML_REJECT_NONE;

    if (top == TINYML_INTENT_UNKNOWN) {
        out->rejected = true;
        out->reject_reason = TINYML_REJECT_UNKNOWN_INTENT;
    } else if (out->confidence < model->confidence_threshold) {
        out->rejected = true;
        out->reject_reason = TINYML_REJECT_LOW_CONFIDENCE;
    } else if (out->margin < model->margin_threshold) {
        out->rejected = true;
        out->reject_reason = TINYML_REJECT_LOW_MARGIN;
    }
    return true;
}

bool tinyml_predict_text(const tinyml_model_t *model,
                         const char *text,
                         tinyml_result_t *out)
{
    tinyml_tokens_t tokens;
    if (tinyml_tokenize(text, &tokens) != TINYML_TEXT_OK) {
        return false;
    }
    return tinyml_predict_tokens(model, &tokens, out);
}

size_t tinyml_extract_features(const tinyml_model_t *model,
                               const tinyml_tokens_t *tokens,
                               tinyml_feature_t *out,
                               size_t capacity)
{
    if (!model_is_valid(model) || tokens == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    tinyml_tokens_t masked;
    make_intent_tokens(tokens, &masked);
    int32_t logits[TINYML_MAX_LABELS] = {0};
    uint16_t feature_count = 0;
    size_t stored = 0;
    accumulate_all(model, &masked, logits, &feature_count, NULL, out, capacity,
                   &stored);

    /* The Python reference reports sorted, aggregated bucket counts. Sort the
     * occurrences in place and merge collisions. Insertion sort is deliberate:
     * this is a test/debug helper, and avoiding qsort keeps the firmware free
     * of hidden heap use. */
    for (size_t i = 1; i < stored; ++i) {
        tinyml_feature_t value = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].bucket > value.bucket) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = value;
    }
    size_t merged = 0;
    for (size_t i = 0; i < stored; ++i) {
        if (merged > 0 && out[merged - 1].bucket == out[i].bucket) {
            if (out[merged - 1].count < UINT16_MAX) {
                ++out[merged - 1].count;
            }
        } else {
            out[merged] = out[i];
            ++merged;
        }
    }
    return merged;
}

size_t tinyml_model_size(const tinyml_model_t *model)
{
    if (model == NULL) {
        return 0;
    }
    return (size_t)model->classes * model->feature_buckets * sizeof(int8_t) +
           (size_t)model->classes * sizeof(int32_t);
}

const char *tinyml_text_status_name(tinyml_text_status_t status)
{
    switch (status) {
    case TINYML_TEXT_OK: return "ok";
    case TINYML_TEXT_EMPTY: return "empty";
    case TINYML_TEXT_TOO_LONG: return "too_long";
    case TINYML_TEXT_NON_ASCII: return "non_ascii";
    case TINYML_TEXT_TOO_MANY_TOKENS: return "too_many_tokens";
    case TINYML_TEXT_TOKEN_TOO_LONG: return "token_too_long";
    case TINYML_TEXT_INVALID_ARGUMENT: return "invalid_argument";
    default: return "unknown";
    }
}

const char *tinyml_intent_name(tinyml_intent_t intent)
{
    switch (intent) {
    case TINYML_INTENT_TURN_ON: return "TURN_ON";
    case TINYML_INTENT_TURN_OFF: return "TURN_OFF";
    case TINYML_INTENT_TOGGLE: return "TOGGLE";
    case TINYML_INTENT_GET_STATUS: return "GET_STATUS";
    case TINYML_INTENT_LIST_DEVICES: return "LIST_DEVICES";
    case TINYML_INTENT_HELP: return "HELP";
    case TINYML_INTENT_UNKNOWN: return "UNKNOWN";
    default: return "INVALID";
    }
}

const char *tinyml_reject_reason_name(tinyml_reject_reason_t reason)
{
    switch (reason) {
    case TINYML_REJECT_NONE: return "none";
    case TINYML_REJECT_UNKNOWN_INTENT: return "unknown_intent";
    case TINYML_REJECT_LOW_CONFIDENCE: return "low_confidence";
    case TINYML_REJECT_LOW_MARGIN: return "low_margin";
    default: return "invalid";
    }
}
