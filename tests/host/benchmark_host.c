/*
 * Host-only TinyML timing probe.
 *
 * These numbers describe the machine that built and ran this file, not an
 * ESP32. Firmware latency must be measured on the target with the "bench"
 * console command. The benchmark deliberately reports wall-clock time and
 * makes no accuracy claim.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "model_data.h"
#include "tinyml.h"

static const char *const k_phrases[] = {
    "turn on the desk light",
    "switch the fan off",
    "toggle gpio twenty-five",
    "what is the temperature sensor status",
    "list every configured device",
    "help",
    "make me a sandwich",
    "please enable the bench lamp",
    "do not turn on the relay",
    "configure pin 23 as an output",
};

static double monotonic_seconds(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void print_prediction(const tinyml_model_t *model, const char *text)
{
    tinyml_result_t result;
    if (!tinyml_predict_text(model, text, &result)) {
        printf("  invalid: %s\n", text);
        return;
    }
    printf("  %-42s intent=%-12s conf=%.3f margin=%.3f rejected=%s\n",
           text, tinyml_intent_name(result.intent), (double)result.confidence,
           (double)result.margin, result.rejected ? "yes" : "no");
}

int main(int argc, char **argv)
{
    const tinyml_model_t *model = model_data_get();
    if (model == NULL) {
        fprintf(stderr, "model unavailable\n");
        return 1;
    }

    if (argc >= 3 && strcmp(argv[1], "--classify") == 0) {
        for (int i = 2; i < argc; ++i) {
            print_prediction(model, argv[i]);
        }
        return 0;
    }

    unsigned iterations = 20000;
    if (argc >= 2) {
        char *end = NULL;
        unsigned long parsed = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || parsed == 0 || parsed > 10000000ul) {
            fprintf(stderr, "usage: %s [iterations] | --classify <text>...\n",
                    argv[0]);
            return 2;
        }
        iterations = (unsigned)parsed;
    }

    const size_t phrase_count = sizeof(k_phrases) / sizeof(k_phrases[0]);

    /* Warm the code and data caches without timing them. */
    volatile float warmup = 0.0f;
    for (unsigned i = 0; i < 200; ++i) {
        tinyml_result_t result;
        if (!tinyml_predict_text(model, k_phrases[i % phrase_count], &result)) {
            fprintf(stderr, "warmup prediction failed\n");
            return 1;
        }
        warmup += result.confidence;
    }

    volatile float checksum = 0.0f;
    unsigned valid = 0;
    double start = monotonic_seconds();
    for (unsigned i = 0; i < iterations; ++i) {
        tinyml_result_t result;
        if (!tinyml_predict_text(model, k_phrases[i % phrase_count], &result)) {
            fprintf(stderr, "prediction failed at iteration %u\n", i);
            return 1;
        }
        checksum += result.confidence;
        ++valid;
    }
    double elapsed = monotonic_seconds() - start;

    if (elapsed <= 0.0) {
        fprintf(stderr, "timer resolution was insufficient; try more iterations\n");
        return 1;
    }

    printf("Host TinyML benchmark (wall clock, single thread)\n");
    printf("  model:          %s\n", model->version);
    printf("  model bytes:    %u\n", (unsigned)model->model_bytes);
    printf("  features:       %u buckets, %u classes\n",
           (unsigned)model->feature_buckets, (unsigned)model->classes);
    printf("  phrases:        %u rotating inputs\n", (unsigned)phrase_count);
    printf("  iterations:     %u\n", valid);
    printf("  elapsed:        %.6f s\n", elapsed);
    printf("  per prediction: %.3f us\n", elapsed * 1000000.0 / (double)valid);
    printf("  throughput:     %.0f predictions/s\n",
           (double)valid / elapsed);
    printf("  checksum:       %.6f\n", (double)checksum);
    printf("These are host numbers, not ESP32 measurements.\n");
    (void)warmup;
    return 0;
}
