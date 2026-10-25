#include "test_framework.h"

#include <math.h>

#include "model_data.h"
#include "tinyai_golden.h"
#include "tinyml.h"

void test_tinyml(void)
{
    TEST_CASE("tinyml tokenizer and feature buckets match Python golden vectors");
    const tinyml_model_t *model = model_data_get();
    TEST_ASSERT(model != NULL);

    for (int i = 0; i < GOLDEN_FEATURE_COUNT; ++i) {
        const golden_feature_case_t *g = &g_feature_golden[i];
        tinyml_tokens_t tokens;
        TEST_EQ_INT(TINYML_TEXT_OK, tinyml_tokenize(g->text, &tokens));
        TEST_EQ_INT(g->token_count, tokens.count);
        for (uint8_t t = 0; t < g->token_count && t < TINYML_MAX_TOKENS; ++t) {
            TEST_EQ_STR(g->tokens[t], tokens.tokens[t]);
        }

        tinyml_feature_t features[TINYML_MAX_FEATURES];
        size_t n = tinyml_extract_features(model, &tokens, features,
                                           TINYML_MAX_FEATURES);
        TEST_EQ_INT(g->feature_count, n);
        for (size_t f = 0; f < n; ++f) {
            TEST_EQ_INT(g->buckets[f], features[f].bucket);
            TEST_EQ_INT(g->counts[f], features[f].count);
        }
    }

    TEST_CASE("tinyml int8 logits and confidence match Python golden vectors");
    for (int i = 0; i < GOLDEN_MODEL_COUNT; ++i) {
        const golden_model_case_t *g = &g_model_golden[i];
        tinyml_result_t result;
        TEST_ASSERT(tinyml_predict_text(model, g->text, &result));
        for (int c = 0; c < TINYML_INTENT_COUNT; ++c) {
            TEST_EQ_INT(g->logits[c], result.logits[c]);
        }
        TEST_EQ_INT(g->predicted_intent, (int)result.intent);
        TEST_EQ_FLOAT(g->confidence, result.confidence, 2e-5);
        TEST_EQ_FLOAT(g->margin, result.margin, 2e-5);
        TEST_EQ_INT(g->rejected, result.rejected);
    }

    TEST_CASE("tinyml rejects malformed and oversized input");
    tinyml_tokens_t tokens;
    TEST_EQ_INT(TINYML_TEXT_INVALID_ARGUMENT, tinyml_tokenize(NULL, &tokens));
    TEST_EQ_INT(TINYML_TEXT_EMPTY, tinyml_tokenize("   ,.;   ", &tokens));
    TEST_EQ_INT(TINYML_TEXT_NON_ASCII, tinyml_tokenize("turn on caf\xC3\xA9", &tokens));

    char long_text[TINYML_MAX_TEXT_BYTES + 8];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    TEST_EQ_INT(TINYML_TEXT_TOO_LONG, tinyml_tokenize(long_text, &tokens));

    char many_tokens[128] = "";
    for (int i = 0; i < TINYML_MAX_TOKENS + 1; ++i) {
        strcat(many_tokens, "a ");
    }
    TEST_EQ_INT(TINYML_TEXT_TOO_MANY_TOKENS, tinyml_tokenize(many_tokens, &tokens));

    char long_token[TINYML_MAX_TOKEN_CHARS + 3];
    memset(long_token, 'b', sizeof(long_token) - 1);
    long_token[sizeof(long_token) - 1] = '\0';
    TEST_EQ_INT(TINYML_TEXT_TOKEN_TOO_LONG, tinyml_tokenize(long_token, &tokens));

    TEST_EQ_INT(TINYML_TEXT_OK, tinyml_tokenize("DON'T toggle LED-1", &tokens));
    TEST_EQ_STR("dont", tokens.tokens[0]);
    TEST_EQ_STR("toggle", tokens.tokens[1]);
    TEST_EQ_STR("led", tokens.tokens[2]);
    TEST_EQ_STR("1", tokens.tokens[3]);
}
