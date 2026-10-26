#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intents.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TINYML_MAX_TEXT_BYTES 160
#define TINYML_MAX_TOKENS 24
#define TINYML_MAX_TOKEN_CHARS 24
#define TINYML_MAX_LABELS INTENT_CLASS_COUNT
#define TINYML_MAX_FEATURES 2048

typedef enum {
    TINYML_INTENT_TURN_ON = 0,
    TINYML_INTENT_TURN_OFF = 1,
    TINYML_INTENT_TOGGLE = 2,
    TINYML_INTENT_GET_STATUS = 3,
    TINYML_INTENT_LIST_DEVICES = 4,
    TINYML_INTENT_HELP = 5,
    TINYML_INTENT_UNKNOWN = 6,
    TINYML_INTENT_COUNT = 7
} tinyml_intent_t;

typedef enum {
    TINYML_TEXT_OK = 0,
    TINYML_TEXT_EMPTY,
    TINYML_TEXT_TOO_LONG,
    TINYML_TEXT_NON_ASCII,
    TINYML_TEXT_TOO_MANY_TOKENS,
    TINYML_TEXT_TOKEN_TOO_LONG,
    TINYML_TEXT_INVALID_ARGUMENT
} tinyml_text_status_t;

typedef enum {
    TINYML_REJECT_NONE = 0,
    TINYML_REJECT_UNKNOWN_INTENT,
    TINYML_REJECT_LOW_CONFIDENCE,
    TINYML_REJECT_LOW_MARGIN
} tinyml_reject_reason_t;

typedef struct {
    uint8_t count;
    char tokens[TINYML_MAX_TOKENS][TINYML_MAX_TOKEN_CHARS + 1];
} tinyml_tokens_t;

typedef struct {
    uint16_t bucket;
    uint16_t count;
} tinyml_feature_t;

typedef struct {
    uint16_t feature_buckets;
    uint8_t classes;
    uint8_t char_ngram_min;
    uint8_t char_ngram_max;
    uint8_t max_tokens;
    uint16_t max_text_bytes;
    const int8_t *weights;       /* [classes][feature_buckets], row-major */
    const int32_t *biases;       /* [classes], quantized in weight units */
    float weight_scale;          /* float logit = int_logit * weight_scale */
    const char *const *labels;
    const char *version;
    uint32_t dataset_hash;
    float confidence_threshold;
    float margin_threshold;
    uint32_t model_bytes;
} tinyml_model_t;

typedef struct {
    tinyml_intent_t intent;
    const char *label;
    float confidence;
    float margin;
    int32_t logits[TINYML_MAX_LABELS];
    uint16_t feature_count;
    bool rejected;
    tinyml_reject_reason_t reject_reason;
} tinyml_result_t;

/* Bounded, ASCII-only tokenizer shared by every layer. Returns a specific
 * status instead of silently truncating. */
tinyml_text_status_t tinyml_tokenize(const char *text, tinyml_tokens_t *out);

/* Run the quantized linear classifier. Returns false only for invalid
 * arguments/model metadata; low confidence is reported via result.rejected. */
bool tinyml_predict_tokens(const tinyml_model_t *model,
                           const tinyml_tokens_t *tokens,
                           tinyml_result_t *out);

bool tinyml_predict_text(const tinyml_model_t *model,
                         const char *text,
                         tinyml_result_t *out);

/* Debug/test helper. Emits each key occurrence in deterministic order. Returns
 * the number stored (capped at capacity), not the uncapped total. */
size_t tinyml_extract_features(const tinyml_model_t *model,
                               const tinyml_tokens_t *tokens,
                               tinyml_feature_t *out,
                               size_t capacity);

uint32_t tinyml_fnv1a32(const char *data);
size_t tinyml_model_size(const tinyml_model_t *model);
const char *tinyml_text_status_name(tinyml_text_status_t status);
const char *tinyml_intent_name(tinyml_intent_t intent);
const char *tinyml_reject_reason_name(tinyml_reject_reason_t reason);

#ifdef __cplusplus
}
#endif
