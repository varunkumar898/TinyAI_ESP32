#include "slot_extractor.h"
#include "validator_hook.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * 1. Intent Table and String Helpers (Derived from intents.def)
 * ========================================================================= */

static const char *const s_intent_names[] = {
#define INTENT_DEF(id, str) str,
#include "intents.def"
#undef INTENT_DEF
};

const char *intent_to_string(intent_t intent)
{
    if ((size_t)intent < sizeof(s_intent_names) / sizeof(s_intent_names[0])) {
        return s_intent_names[intent];
    }
    return "UNKNOWN";
}

intent_t intent_from_string(const char *name)
{
    if (name == NULL) {
        return INTENT_UNKNOWN;
    }
    for (size_t i = 0; i < sizeof(s_intent_names) / sizeof(s_intent_names[0]); ++i) {
        if (strcasecmp(name, s_intent_names[i]) == 0) {
            return (intent_t)i;
        }
    }
    return INTENT_UNKNOWN;
}

bool intent_is_valid_name(const char *name)
{
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(s_intent_names) / sizeof(s_intent_names[0]); ++i) {
        if (strcasecmp(name, s_intent_names[i]) == 0) {
            return true;
        }
    }
    return false;
}

const char *intent_get_by_index(size_t index)
{
    if (index < sizeof(s_intent_names) / sizeof(s_intent_names[0])) {
        return s_intent_names[index];
    }
    return NULL;
}

/* =========================================================================
 * 2. Typed Command Constructors (Zero-Initializing)
 * ========================================================================= */

typed_command_t cmd_make_gpio_write(uint8_t pin, uint8_t level)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_GPIO_WRITE;
    cmd.params.gpio_write.pin = pin;
    cmd.params.gpio_write.level = level ? 1 : 0;
    return cmd;
}

typed_command_t cmd_make_gpio_toggle(uint8_t pin)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_GPIO_TOGGLE;
    cmd.params.gpio_toggle.pin = pin;
    return cmd;
}

typed_command_t cmd_make_gpio_read(uint8_t pin)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_GPIO_READ;
    cmd.params.gpio_read.pin = pin;
    return cmd;
}

typed_command_t cmd_make_gpio_blink(uint8_t pin, uint32_t on_ms, uint32_t off_ms, uint32_t repeats)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_GPIO_BLINK;
    cmd.params.gpio_blink.pin = pin;
    cmd.params.gpio_blink.on_ms = on_ms;
    cmd.params.gpio_blink.off_ms = (off_ms > 0) ? off_ms : on_ms;
    cmd.params.gpio_blink.repeats = (repeats > 0) ? repeats : 1;
    return cmd;
}

typed_command_t cmd_make_gpio_pulse(uint8_t pin, uint32_t duration_ms)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_GPIO_PULSE;
    cmd.params.gpio_pulse.pin = pin;
    cmd.params.gpio_pulse.duration_ms = duration_ms;
    return cmd;
}

typed_command_t cmd_make_pwm(uint8_t pin, uint32_t freq_hz, uint8_t duty_pct)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_PWM;
    cmd.params.pwm.pin = pin;
    cmd.params.pwm.freq_hz = freq_hz;
    cmd.params.pwm.duty_pct = (duty_pct > 100) ? 100 : duty_pct;
    return cmd;
}

typed_command_t cmd_make_adc_read(uint8_t pin, bool raw_counts)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_ADC_READ;
    cmd.params.adc_read.pin = pin;
    cmd.params.adc_read.raw_counts = raw_counts;
    return cmd;
}

typed_command_t cmd_make_i2c_scan(uint8_t bus)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_I2C_SCAN;
    cmd.params.i2c_scan.bus = bus;
    return cmd;
}

typed_command_t cmd_make_uart_send(uint8_t uart_id, uint32_t baud, const char *payload, size_t length)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_UART_SEND;
    cmd.params.uart_send.uart_id = uart_id;
    cmd.params.uart_send.baud = baud ? baud : 115200;
    if (payload != NULL) {
        size_t copy_len = length < (CMD_UART_PAYLOAD_MAX - 1) ? length : (CMD_UART_PAYLOAD_MAX - 1);
        memcpy(cmd.params.uart_send.payload, payload, copy_len);
        cmd.params.uart_send.payload[copy_len] = '\0';
        cmd.params.uart_send.length = copy_len;
    }
    return cmd;
}

typed_command_t cmd_make_get_status(int16_t pin)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_GET_STATUS;
    cmd.params.get_status.pin = pin;
    return cmd;
}

typed_command_t cmd_make_help(const char *topic)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_HELP;
    if (topic != NULL) {
        strncpy(cmd.params.help.topic, topic, sizeof(cmd.params.help.topic) - 1);
    }
    return cmd;
}

typed_command_t cmd_make_unknown(const char *reason)
{
    typed_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.intent = INTENT_UNKNOWN;
    if (reason != NULL) {
        strncpy(cmd.params.unknown.reason, reason, sizeof(cmd.params.unknown.reason) - 1);
    }
    return cmd;
}

/* =========================================================================
 * 3. Static Capability Table and Validator Hook
 * ========================================================================= */

static const uint32_t s_esp32_pin_caps[40] = {
    /* 0 */  PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM | PIN_CAP_PROTECTED,
    /* 1 */  PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_CONSOLE,
    /* 2 */  PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM | PIN_CAP_PROTECTED,
    /* 3 */  PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_CONSOLE,
    /* 4 */  PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 5 */  PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM | PIN_CAP_PROTECTED,
    /* 6..11 Flash pins */
    /* 6 */  PIN_CAP_FLASH,
    /* 7 */  PIN_CAP_FLASH,
    /* 8 */  PIN_CAP_FLASH,
    /* 9 */  PIN_CAP_FLASH,
    /* 10 */ PIN_CAP_FLASH,
    /* 11 */ PIN_CAP_FLASH,
    /* 12 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM | PIN_CAP_PROTECTED,
    /* 13 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 14 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 15 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM | PIN_CAP_PROTECTED,
    /* 16 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 17 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 18 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 19 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 20 */ PIN_CAP_NONE, /* Absent on ESP32 */
    /* 21 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 22 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 23 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 24 */ PIN_CAP_NONE, /* Absent on ESP32 */
    /* 25 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM | PIN_CAP_DAC,
    /* 26 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM | PIN_CAP_DAC,
    /* 27 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM,
    /* 28..31 Absent */
    /* 28 */ PIN_CAP_NONE,
    /* 29 */ PIN_CAP_NONE,
    /* 30 */ PIN_CAP_NONE,
    /* 31 */ PIN_CAP_NONE,
    /* 32 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM | PIN_CAP_ADC1,
    /* 33 */ PIN_CAP_OUTPUT | PIN_CAP_INPUT | PIN_CAP_PWM | PIN_CAP_ADC1,
    /* 34..39 Input-only ADC1 */
    /* 34 */ PIN_CAP_INPUT | PIN_CAP_ADC1,
    /* 35 */ PIN_CAP_INPUT | PIN_CAP_ADC1,
    /* 36 */ PIN_CAP_INPUT | PIN_CAP_ADC1,
    /* 37 */ PIN_CAP_INPUT | PIN_CAP_ADC1,
    /* 38 */ PIN_CAP_INPUT | PIN_CAP_ADC1,
    /* 39 */ PIN_CAP_INPUT | PIN_CAP_ADC1,
};

uint32_t validator_get_pin_caps(uint8_t pin)
{
    if (pin >= 40) {
        return PIN_CAP_NONE;
    }
    return s_esp32_pin_caps[pin];
}

validator_result_t validator_validate(const typed_command_t *cmd, bool allow_protected)
{
    validator_result_t res;
    memset(&res, 0, sizeof(res));
    res.status = VALIDATE_OK;

    if (cmd == NULL) {
        res.status = VALIDATE_ERR_UNSUPPORTED_INTENT;
        snprintf(res.reason, sizeof(res.reason), "null command");
        return res;
    }

    switch (cmd->intent) {
    case INTENT_GPIO_WRITE:
    case INTENT_GPIO_TOGGLE:
    case INTENT_GPIO_BLINK:
    case INTENT_GPIO_PULSE: {
        uint8_t pin = 0;
        if (cmd->intent == INTENT_GPIO_WRITE) pin = cmd->params.gpio_write.pin;
        else if (cmd->intent == INTENT_GPIO_TOGGLE) pin = cmd->params.gpio_toggle.pin;
        else if (cmd->intent == INTENT_GPIO_BLINK) pin = cmd->params.gpio_blink.pin;
        else if (cmd->intent == INTENT_GPIO_PULSE) pin = cmd->params.gpio_pulse.pin;

        if (pin >= 40 || s_esp32_pin_caps[pin] == PIN_CAP_NONE) {
            res.status = VALIDATE_ERR_NONEXISTENT_PIN;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u does not exist on ESP32", pin);
            return res;
        }
        uint32_t caps = s_esp32_pin_caps[pin];
        if (caps & PIN_CAP_FLASH) {
            res.status = VALIDATE_ERR_FLASH_BLOCKED;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is hard-blocked (SPI flash)", pin);
            return res;
        }
        if (caps & PIN_CAP_CONSOLE) {
            res.status = VALIDATE_ERR_CONSOLE_BLOCKED;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is hard-blocked (UART0 console)", pin);
            return res;
        }
        if ((caps & PIN_CAP_PROTECTED) && !allow_protected) {
            res.status = VALIDATE_ERR_PROTECTED_PIN;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is a protected strapping pin", pin);
            return res;
        }
        if (!(caps & PIN_CAP_OUTPUT)) {
            res.status = VALIDATE_ERR_INPUT_ONLY;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is input-only", pin);
            return res;
        }
        break;
    }
    case INTENT_GPIO_READ: {
        uint8_t pin = cmd->params.gpio_read.pin;
        if (pin >= 40 || s_esp32_pin_caps[pin] == PIN_CAP_NONE) {
            res.status = VALIDATE_ERR_NONEXISTENT_PIN;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u does not exist", pin);
            return res;
        }
        uint32_t caps = s_esp32_pin_caps[pin];
        if (caps & PIN_CAP_FLASH) {
            res.status = VALIDATE_ERR_FLASH_BLOCKED;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is flash-blocked", pin);
            return res;
        }
        if (caps & PIN_CAP_CONSOLE) {
            res.status = VALIDATE_ERR_CONSOLE_BLOCKED;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is console-blocked", pin);
            return res;
        }
        break;
    }
    case INTENT_PWM: {
        uint8_t pin = cmd->params.pwm.pin;
        if (pin >= 40 || s_esp32_pin_caps[pin] == PIN_CAP_NONE) {
            res.status = VALIDATE_ERR_NONEXISTENT_PIN;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u does not exist", pin);
            return res;
        }
        uint32_t caps = s_esp32_pin_caps[pin];
        if (caps & PIN_CAP_FLASH) {
            res.status = VALIDATE_ERR_FLASH_BLOCKED;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is flash-blocked", pin);
            return res;
        }
        if (caps & PIN_CAP_CONSOLE) {
            res.status = VALIDATE_ERR_CONSOLE_BLOCKED;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is console-blocked", pin);
            return res;
        }
        if ((caps & PIN_CAP_PROTECTED) && !allow_protected) {
            res.status = VALIDATE_ERR_PROTECTED_PIN;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is protected strapping pin", pin);
            return res;
        }
        if (!(caps & PIN_CAP_OUTPUT)) {
            res.status = VALIDATE_ERR_INPUT_ONLY;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is input-only (no PWM)", pin);
            return res;
        }
        if (cmd->params.pwm.freq_hz < EXTRACTOR_MIN_LEDC_FREQ ||
            cmd->params.pwm.freq_hz > EXTRACTOR_MAX_LEDC_FREQ) {
            res.status = VALIDATE_ERR_INVALID_PWM;
            snprintf(res.reason, sizeof(res.reason), "PWM frequency %lu Hz outside LEDC range",
                     (unsigned long)cmd->params.pwm.freq_hz);
            return res;
        }
        if (cmd->params.pwm.duty_pct > 100) {
            res.status = VALIDATE_ERR_INVALID_PWM;
            snprintf(res.reason, sizeof(res.reason), "PWM duty %u%% outside 0-100",
                     cmd->params.pwm.duty_pct);
            return res;
        }
        break;
    }
    case INTENT_ADC_READ: {
        uint8_t pin = cmd->params.adc_read.pin;
        if (pin >= 40 || s_esp32_pin_caps[pin] == PIN_CAP_NONE) {
            res.status = VALIDATE_ERR_NONEXISTENT_PIN;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u does not exist", pin);
            return res;
        }
        uint32_t caps = s_esp32_pin_caps[pin];
        if (!(caps & PIN_CAP_ADC1)) {
            res.status = VALIDATE_ERR_NOT_ADC1;
            snprintf(res.reason, sizeof(res.reason), "GPIO%u is not an ADC1 pin (ADC restricted to ADC1 GPIO32-39)", pin);
            return res;
        }
        break;
    }
    case INTENT_UART_SEND: {
        if (cmd->params.uart_send.uart_id > 2) {
            res.status = VALIDATE_ERR_INVALID_UART;
            snprintf(res.reason, sizeof(res.reason), "UART%u does not exist (ESP32 has UART 0-2)",
                     cmd->params.uart_send.uart_id);
            return res;
        }
        break;
    }
    case INTENT_I2C_SCAN:
    case INTENT_GET_STATUS:
    case INTENT_HELP:
    case INTENT_UNKNOWN:
        break;
    default:
        res.status = VALIDATE_ERR_UNSUPPORTED_INTENT;
        snprintf(res.reason, sizeof(res.reason), "Unsupported intent %d", (int)cmd->intent);
        return res;
    }

    return res;
}

const char *validator_status_name(validator_status_t status)
{
    switch (status) {
    case VALIDATE_OK: return "OK";
    case VALIDATE_ERR_NONEXISTENT_PIN: return "NONEXISTENT_PIN";
    case VALIDATE_ERR_FLASH_BLOCKED: return "FLASH_BLOCKED";
    case VALIDATE_ERR_CONSOLE_BLOCKED: return "CONSOLE_BLOCKED";
    case VALIDATE_ERR_PROTECTED_PIN: return "PROTECTED_PIN";
    case VALIDATE_ERR_INPUT_ONLY: return "INPUT_ONLY";
    case VALIDATE_ERR_NOT_ADC1: return "NOT_ADC1";
    case VALIDATE_ERR_NOT_DAC: return "NOT_DAC";
    case VALIDATE_ERR_INVALID_PWM: return "INVALID_PWM";
    case VALIDATE_ERR_INVALID_UART: return "INVALID_UART";
    default: return "UNSUPPORTED_INTENT";
    }
}

/* =========================================================================
 * 4. Number Words and Written Number Parser
 * ========================================================================= */

typedef struct {
    const char *word;
    uint32_t val;
} word_entry_t;

static const word_entry_t s_units_and_teens[] = {
    {"zero", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4},
    {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9},
    {"ten", 10}, {"eleven", 11}, {"twelve", 12}, {"thirteen", 13},
    {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17},
    {"eighteen", 18}, {"nineteen", 19},
};

static const word_entry_t s_tens[] = {
    {"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50},
    {"sixty", 60}, {"seventy", 70}, {"eighty", 80}, {"ninety", 90},
};

static int lookup_word_val(const char *word, uint32_t *val)
{
    for (size_t i = 0; i < sizeof(s_units_and_teens) / sizeof(s_units_and_teens[0]); ++i) {
        if (strcmp(word, s_units_and_teens[i].word) == 0) {
            *val = s_units_and_teens[i].val;
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof(s_tens) / sizeof(s_tens[0]); ++i) {
        if (strcmp(word, s_tens[i].word) == 0) {
            *val = s_tens[i].val;
            return 2;
        }
    }
    if (strcmp(word, "hundred") == 0) {
        *val = 100;
        return 3;
    }
    if (strcmp(word, "thousand") == 0) {
        *val = 1000;
        return 4;
    }
    if (strcmp(word, "million") == 0) {
        *val = 1000000;
        return 5;
    }
    return 0;
}

bool extractor_parse_written_number(const char *text, uint64_t *out_val)
{
    if (text == NULL || out_val == NULL) {
        return false;
    }

    /* Tokenize locally */
    char buf[128];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint64_t total = 0;
    uint64_t current = 0;
    bool found_any = false;

    char *saveptr = NULL;
    char *token = strtok_r(buf, " -", &saveptr);
    while (token != NULL) {
        /* Convert token to lowercase */
        for (char *p = token; *p; ++p) *p = (char)tolower((unsigned char)*p);

        uint32_t val = 0;
        int type = lookup_word_val(token, &val);
        if (type == 0) {
            return false; /* Unrecognized number word */
        }
        found_any = true;

        if (type == 1 || type == 2) {
            /* Units, teens, tens */
            current += val;
        } else if (type == 3) {
            /* Hundred */
            if (current == 0) current = 1;
            current *= 100;
        } else if (type == 4) {
            /* Thousand */
            if (current == 0) current = 1;
            total += current * 1000ULL;
            current = 0;
        } else if (type == 5) {
            /* Million */
            if (current == 0) current = 1;
            total += current * 1000000ULL;
            current = 0;
        }
        token = strtok_r(NULL, " -", &saveptr);
    }

    if (!found_any) {
        return false;
    }
    *out_val = total + current;
    return true;
}

/* =========================================================================
 * 5. Normalization and Masking
 * ========================================================================= */

static bool is_ascii(unsigned char c)
{
    return c < 0x80;
}

bool extractor_normalize(const char *input, extractor_normalized_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (input == NULL || input[0] == '\0') {
        out->safety = SAFETY_REJECT_EMPTY;
        snprintf(out->safety_reason, sizeof(out->safety_reason), "Input is empty");
        return false;
    }

    size_t len = strlen(input);
    if (len > EXTRACTOR_MAX_TEXT_LEN) {
        out->safety = SAFETY_REJECT_TOO_LONG;
        snprintf(out->safety_reason, sizeof(out->safety_reason), "Input exceeds max length (%u bytes)",
                 (unsigned)EXTRACTOR_MAX_TEXT_LEN);
        return false;
    }

    /* Check ASCII */
    for (size_t i = 0; i < len; ++i) {
        if (!is_ascii((unsigned char)input[i])) {
            out->safety = SAFETY_REJECT_NON_ASCII;
            snprintf(out->safety_reason, sizeof(out->safety_reason), "Non-ASCII characters detected");
            return false;
        }
    }

    /* Pass 1: Lowercase, normalize punctuation and spacing into intermediate buffer */
    char step1[EXTRACTOR_MAX_TEXT_LEN * 2 + 16];
    size_t step1_idx = 0;
    bool last_was_space = true;

    for (size_t i = 0; i < len; ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }

        if (c == '\'') {
            /* don't -> dont */
            continue;
        }

        if (c == ';' || c == '\n' || c == '\r') {
            /* Semicolons / newlines are preserved as separate token for multi-command check */
            if (!last_was_space && step1_idx < sizeof(step1) - 4) {
                step1[step1_idx++] = ' ';
            }
            if (step1_idx < sizeof(step1) - 4) {
                step1[step1_idx++] = ';';
                step1[step1_idx++] = ' ';
                last_was_space = true;
            }
            continue;
        }

        if (isalnum((unsigned char)c) || c == '%') {
            if (step1_idx < sizeof(step1) - 2) {
                step1[step1_idx++] = c;
                last_was_space = false;
            }
        } else if (c == '-') {
            /* Treat hyphen between words as space */
            if (!last_was_space && step1_idx < sizeof(step1) - 2) {
                step1[step1_idx++] = ' ';
                last_was_space = true;
            }
        } else {
            /* Strip punctuation (. , ! ? _ " etc.) -> space */
            if (!last_was_space && step1_idx < sizeof(step1) - 2) {
                step1[step1_idx++] = ' ';
                last_was_space = true;
            }
        }
    }
    while (step1_idx > 0 && step1[step1_idx - 1] == ' ') {
        step1_idx--;
    }
    step1[step1_idx] = '\0';

    if (step1_idx == 0) {
        out->safety = SAFETY_REJECT_EMPTY;
        snprintf(out->safety_reason, sizeof(out->safety_reason), "Input contains no alphanumeric content");
        return false;
    }

    /* Tokenize step1 into tokens */
    char tokens[EXTRACTOR_MAX_TOKENS][EXTRACTOR_MAX_TOKEN_CHARS];
    size_t num_tokens = 0;
    char step1_copy[sizeof(step1)];
    memcpy(step1_copy, step1, sizeof(step1));

    char *saveptr = NULL;
    char *tok = strtok_r(step1_copy, " ", &saveptr);
    while (tok != NULL && num_tokens < EXTRACTOR_MAX_TOKENS) {
        strncpy(tokens[num_tokens], tok, EXTRACTOR_MAX_TOKEN_CHARS - 1);
        tokens[num_tokens][EXTRACTOR_MAX_TOKEN_CHARS - 1] = '\0';
        num_tokens++;
        tok = strtok_r(NULL, " ", &saveptr);
    }

    /* Pass 2: Written numbers to digits & Alias normalization */
    char norm_tokens[EXTRACTOR_MAX_TOKENS][EXTRACTOR_MAX_TOKEN_CHARS];
    size_t norm_count = 0;

    for (size_t i = 0; i < num_tokens && norm_count < EXTRACTOR_MAX_TOKENS; ) {
        /* Check built-in LED alias */
        if (strcmp(tokens[i], "builtin") == 0 || strcmp(tokens[i], "built") == 0) {
            size_t next = i + 1;
            if (strcmp(tokens[i], "built") == 0 && next < num_tokens && strcmp(tokens[next], "in") == 0) {
                next++;
            }
            if (next < num_tokens && strcmp(tokens[next], "led") == 0) {
                strncpy(norm_tokens[norm_count++], "gpio2", EXTRACTOR_MAX_TOKEN_CHARS - 1);
                i = next + 1;
                continue;
            }
        }
        if (strcmp(tokens[i], "onboard") == 0 && (i + 1) < num_tokens && strcmp(tokens[i + 1], "led") == 0) {
            strncpy(norm_tokens[norm_count++], "gpio2", EXTRACTOR_MAX_TOKEN_CHARS - 1);
            i += 2;
            continue;
        }

        /* Check pin/gpio alias: "pin 2", "gpio 2", "io 2" -> "gpio2" */
        if ((strcmp(tokens[i], "pin") == 0 || strcmp(tokens[i], "gpio") == 0 || strcmp(tokens[i], "io") == 0) &&
            (i + 1) < num_tokens) {
            /* Check if next token starts a written number sequence */
            uint32_t val = 0;
            if (lookup_word_val(tokens[i + 1], &val) > 0) {
                uint64_t accumulated_pin = 0;
                size_t start = i + 1;
                size_t cursor = start;
                while (cursor < num_tokens && lookup_word_val(tokens[cursor], &val) > 0) {
                    cursor++;
                }
                char number_str[128] = {0};
                for (size_t k = start; k < cursor; ++k) {
                    strncat(number_str, tokens[k], sizeof(number_str) - strlen(number_str) - 2);
                    if (k + 1 < cursor) strncat(number_str, " ", sizeof(number_str) - strlen(number_str) - 1);
                }
                if (extractor_parse_written_number(number_str, &accumulated_pin) && accumulated_pin < 256) {
                    snprintf(norm_tokens[norm_count++], EXTRACTOR_MAX_TOKEN_CHARS, "gpio%llu",
                             (unsigned long long)accumulated_pin);
                    i = cursor;
                    continue;
                }
            }
            char *endp = NULL;
            unsigned long num = strtoul(tokens[i + 1], &endp, 10);
            if (endp != tokens[i + 1] && *endp == '\0' && num < 256) {
                snprintf(norm_tokens[norm_count++], EXTRACTOR_MAX_TOKEN_CHARS, "gpio%lu", num);
                i += 2;
                continue;
            }
        }

        /* Check "pin2", "io2", "gpio2" attached */
        if ((strncmp(tokens[i], "gpio", 4) == 0 && isdigit((unsigned char)tokens[i][4])) ||
            (strncmp(tokens[i], "pin", 3) == 0 && isdigit((unsigned char)tokens[i][3])) ||
            (strncmp(tokens[i], "io", 2) == 0 && isdigit((unsigned char)tokens[i][2]))) {
            const char *digits_ptr = (tokens[i][0] == 'g') ? tokens[i] + 4 :
                                     (tokens[i][0] == 'p') ? tokens[i] + 3 : tokens[i] + 2;
            snprintf(norm_tokens[norm_count++], EXTRACTOR_MAX_TOKEN_CHARS, "gpio%s", digits_ptr);
            i++;
            continue;
        }

        /* Check written number sequence */
        uint32_t test_val = 0;
        if (lookup_word_val(tokens[i], &test_val) > 0) {
            /* Collect all consecutive number words */
            uint64_t accumulated_number = 0;
            size_t start = i;
            while (i < num_tokens && lookup_word_val(tokens[i], &test_val) > 0) {
                i++;
            }
            /* Build string from start to i */
            char number_str[128] = {0};
            for (size_t k = start; k < i; ++k) {
                strncat(number_str, tokens[k], sizeof(number_str) - strlen(number_str) - 2);
                if (k + 1 < i) strncat(number_str, " ", sizeof(number_str) - strlen(number_str) - 1);
            }
            if (extractor_parse_written_number(number_str, &accumulated_number)) {
                snprintf(norm_tokens[norm_count++], EXTRACTOR_MAX_TOKEN_CHARS, "%llu",
                         (unsigned long long)accumulated_number);
                continue;
            }
        }

        /* Unit normalization */
        if (strcmp(tokens[i], "milliseconds") == 0 || strcmp(tokens[i], "millisecond") == 0 ||
            strcmp(tokens[i], "msec") == 0) {
            strncpy(norm_tokens[norm_count++], "ms", EXTRACTOR_MAX_TOKEN_CHARS - 1);
            i++;
            continue;
        }
        if (strcmp(tokens[i], "seconds") == 0 || strcmp(tokens[i], "second") == 0 ||
            strcmp(tokens[i], "sec") == 0) {
            strncpy(norm_tokens[norm_count++], "s", EXTRACTOR_MAX_TOKEN_CHARS - 1);
            i++;
            continue;
        }
        if (strcmp(tokens[i], "percent") == 0 || strcmp(tokens[i], "pct") == 0) {
            strncpy(norm_tokens[norm_count++], "%", EXTRACTOR_MAX_TOKEN_CHARS - 1);
            i++;
            continue;
        }
        if (strcmp(tokens[i], "hertz") == 0) {
            strncpy(norm_tokens[norm_count++], "hz", EXTRACTOR_MAX_TOKEN_CHARS - 1);
            i++;
            continue;
        }
        if (strcmp(tokens[i], "kilohertz") == 0) {
            strncpy(norm_tokens[norm_count++], "khz", EXTRACTOR_MAX_TOKEN_CHARS - 1);
            i++;
            continue;
        }
        if (strcmp(tokens[i], "megahertz") == 0) {
            strncpy(norm_tokens[norm_count++], "mhz", EXTRACTOR_MAX_TOKEN_CHARS - 1);
            i++;
            continue;
        }

        /* Attached units e.g. "1000ms", "50hz", "2s", "50%" */
        size_t tlen = strlen(tokens[i]);
        if (tlen > 1 && isdigit((unsigned char)tokens[i][0])) {
            char val_buf[EXTRACTOR_MAX_TOKEN_CHARS] = {0};
            char unit_buf[EXTRACTOR_MAX_TOKEN_CHARS] = {0};
            size_t d_idx = 0;
            while (d_idx < tlen && isdigit((unsigned char)tokens[i][d_idx])) {
                val_buf[d_idx] = tokens[i][d_idx];
                d_idx++;
            }
            val_buf[d_idx] = '\0';
            if (d_idx < tlen) {
                strncpy(unit_buf, &tokens[i][d_idx], sizeof(unit_buf) - 1);
                /* Normalize attached unit */
                if (strcmp(unit_buf, "ms") == 0 || strcmp(unit_buf, "milliseconds") == 0 || strcmp(unit_buf, "msec") == 0) {
                    strncpy(norm_tokens[norm_count++], val_buf, EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    if (norm_count < EXTRACTOR_MAX_TOKENS) strncpy(norm_tokens[norm_count++], "ms", EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    i++;
                    continue;
                }
                if (strcmp(unit_buf, "s") == 0 || strcmp(unit_buf, "sec") == 0 || strcmp(unit_buf, "seconds") == 0) {
                    strncpy(norm_tokens[norm_count++], val_buf, EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    if (norm_count < EXTRACTOR_MAX_TOKENS) strncpy(norm_tokens[norm_count++], "s", EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    i++;
                    continue;
                }
                if (strcmp(unit_buf, "hz") == 0 || strcmp(unit_buf, "hertz") == 0) {
                    strncpy(norm_tokens[norm_count++], val_buf, EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    if (norm_count < EXTRACTOR_MAX_TOKENS) strncpy(norm_tokens[norm_count++], "hz", EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    i++;
                    continue;
                }
                if (strcmp(unit_buf, "khz") == 0 || strcmp(unit_buf, "kilohertz") == 0) {
                    strncpy(norm_tokens[norm_count++], val_buf, EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    if (norm_count < EXTRACTOR_MAX_TOKENS) strncpy(norm_tokens[norm_count++], "khz", EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    i++;
                    continue;
                }
                if (strcmp(unit_buf, "mhz") == 0 || strcmp(unit_buf, "megahertz") == 0) {
                    strncpy(norm_tokens[norm_count++], val_buf, EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    if (norm_count < EXTRACTOR_MAX_TOKENS) strncpy(norm_tokens[norm_count++], "mhz", EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    i++;
                    continue;
                }
                if (strcmp(unit_buf, "%") == 0 || strcmp(unit_buf, "percent") == 0 || strcmp(unit_buf, "pct") == 0) {
                    strncpy(norm_tokens[norm_count++], val_buf, EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    if (norm_count < EXTRACTOR_MAX_TOKENS) strncpy(norm_tokens[norm_count++], "%", EXTRACTOR_MAX_TOKEN_CHARS - 1);
                    i++;
                    continue;
                }
            }
        }

        /* Default: keep token */
        strncpy(norm_tokens[norm_count++], tokens[i], EXTRACTOR_MAX_TOKEN_CHARS - 1);
        norm_tokens[norm_count - 1][EXTRACTOR_MAX_TOKEN_CHARS - 1] = '\0';
        i++;
    }

    /* Build normalized string */
    out->normalized[0] = '\0';
    for (size_t i = 0; i < norm_count; ++i) {
        strncat(out->normalized, norm_tokens[i], EXTRACTOR_MAX_TEXT_LEN - strlen(out->normalized) - 1);
        if (i + 1 < norm_count) {
            strncat(out->normalized, " ", EXTRACTOR_MAX_TEXT_LEN - strlen(out->normalized) - 1);
        }
    }

    /* Build masked string */
    out->masked[0] = '\0';
    for (size_t i = 0; i < norm_count; ) {
        const char *curr = norm_tokens[i];
        const char *next = (i + 1 < norm_count) ? norm_tokens[i + 1] : NULL;

        /* Check <PIN> */
        if (strncmp(curr, "gpio", 4) == 0 && isdigit((unsigned char)curr[4])) {
            strncat(out->masked, "<PIN>", EXTRACTOR_MAX_TEXT_LEN - strlen(out->masked) - 1);
            i++;
        }
        /* Check <DUR> */
        else if (next && (isdigit((unsigned char)curr[0])) && (strcmp(next, "ms") == 0 || strcmp(next, "s") == 0)) {
            strncat(out->masked, "<DUR>", EXTRACTOR_MAX_TEXT_LEN - strlen(out->masked) - 1);
            i += 2;
        }
        /* Check <FREQ> */
        else if (next && (isdigit((unsigned char)curr[0])) &&
                 (strcmp(next, "hz") == 0 || strcmp(next, "khz") == 0 || strcmp(next, "mhz") == 0)) {
            strncat(out->masked, "<FREQ>", EXTRACTOR_MAX_TEXT_LEN - strlen(out->masked) - 1);
            i += 2;
        }
        /* Check <PCT> */
        else if (next && (isdigit((unsigned char)curr[0])) && (strcmp(next, "%") == 0)) {
            strncat(out->masked, "<PCT>", EXTRACTOR_MAX_TEXT_LEN - strlen(out->masked) - 1);
            i += 2;
        }
        /* Check standalone <NUM> */
        else if (isdigit((unsigned char)curr[0])) {
            strncat(out->masked, "<NUM>", EXTRACTOR_MAX_TEXT_LEN - strlen(out->masked) - 1);
            i++;
        }
        else {
            strncat(out->masked, curr, EXTRACTOR_MAX_TEXT_LEN - strlen(out->masked) - 1);
            i++;
        }

        if (i < norm_count) {
            strncat(out->masked, " ", EXTRACTOR_MAX_TEXT_LEN - strlen(out->masked) - 1);
        }
    }

    out->safety = SAFETY_PASS;
    return true;
}

/* =========================================================================
 * 6. Rule-based Safety Filter
 * ========================================================================= */

static bool has_exact_token(const char *text, const char *token)
{
    if (text == NULL || token == NULL) return false;
    size_t tlen = strlen(token);
    const char *p = text;
    while ((p = strstr(p, token)) != NULL) {
        bool left_boundary = (p == text || *(p - 1) == ' ');
        bool right_boundary = (*(p + tlen) == '\0' || *(p + tlen) == ' ');
        if (left_boundary && right_boundary) {
            return true;
        }
        p += tlen;
    }
    return false;
}

static bool has_phrase_str(const char *text, const char *phrase)
{
    if (text == NULL || phrase == NULL) return false;
    return (strstr(text, phrase) != NULL);
}

safety_check_t extractor_check_safety(const char *normalized_text)
{
    safety_check_t chk;
    memset(&chk, 0, sizeof(chk));
    chk.safety = SAFETY_PASS;

    if (normalized_text == NULL || normalized_text[0] == '\0') {
        chk.safety = SAFETY_REJECT_EMPTY;
        snprintf(chk.reason, sizeof(chk.reason), "Empty input");
        return chk;
    }

    /* 1. Multi-command chaining check */
    if (strchr(normalized_text, ';') != NULL ||
        has_phrase_str(normalized_text, "and also") ||
        has_phrase_str(normalized_text, "then turn") ||
        has_phrase_str(normalized_text, "then blink") ||
        has_phrase_str(normalized_text, "then pulse") ||
        has_phrase_str(normalized_text, "then toggle")) {
        chk.safety = SAFETY_REJECT_MULTI_COMMAND;
        snprintf(chk.reason, sizeof(chk.reason), "Multi-command chaining detected");
        return chk;
    }

    /* 2. Injection Patterns */
    static const char *const s_injection_phrases[] = {
        "ignore previous", "ignore all", "you are now", "disable safety",
        "bypass", "authorize", "as root", "as admin", "write register",
        "poke", "flash erase", "forget your rules", "forget the rules",
        "forget all rules", "sudo", "jailbreak", "unfiltered", "admin mode",
        "developer mode", "role play", "act as root", "prompt injection",
    };
    for (size_t i = 0; i < sizeof(s_injection_phrases) / sizeof(s_injection_phrases[0]); ++i) {
        if (has_phrase_str(normalized_text, s_injection_phrases[i])) {
            chk.safety = SAFETY_REJECT_INJECTION;
            snprintf(chk.reason, sizeof(chk.reason), "Prompt injection or privilege escalation: '%s'",
                     s_injection_phrases[i]);
            return chk;
        }
    }

    /* 3. Negation Patterns */
    static const char *const s_negation_words[] = {
        "dont", "not", "never", "without", "cannot", "cant",
    };
    for (size_t i = 0; i < sizeof(s_negation_words) / sizeof(s_negation_words[0]); ++i) {
        if (has_exact_token(normalized_text, s_negation_words[i])) {
            chk.safety = SAFETY_REJECT_NEGATION;
            snprintf(chk.reason, sizeof(chk.reason), "Negation detected: '%s'", s_negation_words[i]);
            return chk;
        }
    }
    if (has_phrase_str(normalized_text, "stop short of") ||
        (has_phrase_str(normalized_text, "cancel") &&
         (has_phrase_str(normalized_text, "blink") || has_phrase_str(normalized_text, "turning") ||
          has_phrase_str(normalized_text, "write") || has_phrase_str(normalized_text, "pulse") ||
          has_phrase_str(normalized_text, "toggle")))) {
        chk.safety = SAFETY_REJECT_NEGATION;
        snprintf(chk.reason, sizeof(chk.reason), "Negated actuation detected");
        return chk;
    }

    /* 4. Questions without Action (Informational) */
    static const char *const s_question_phrases[] = {
        "what does", "can you", "how do", "how to", "how does", "would it",
        "could you", "what happens", "what is", "is pin", "is gpio", "how can",
    };
    for (size_t i = 0; i < sizeof(s_question_phrases) / sizeof(s_question_phrases[0]); ++i) {
        if (has_phrase_str(normalized_text, s_question_phrases[i])) {
            chk.safety = SAFETY_REJECT_QUESTION;
            snprintf(chk.reason, sizeof(chk.reason), "Question without actuation: '%s'", s_question_phrases[i]);
            return chk;
        }
    }

    /* 5. Configuration commands (Local console only) */
    static const char *const s_config_words[] = {
        "configure", "config", "setup", "initialize", "initialise", "reconfigure",
    };
    for (size_t i = 0; i < sizeof(s_config_words) / sizeof(s_config_words[0]); ++i) {
        if (has_exact_token(normalized_text, s_config_words[i])) {
            chk.safety = SAFETY_REJECT_CONFIGURATION;
            snprintf(chk.reason, sizeof(chk.reason), "Configuration verb '%s' is local console only", s_config_words[i]);
            return chk;
        }
    }

    return chk;
}

/* =========================================================================
 * 7. Slot Extractor Implementation
 * ========================================================================= */

slot_result_t extractor_parse_command(const char *input)
{
    slot_result_t res;
    memset(&res, 0, sizeof(res));
    res.command.intent = INTENT_UNKNOWN;

    /* 1. Normalize */
    extractor_normalized_t norm;
    if (!extractor_normalize(input, &norm)) {
        res.status = SLOT_STATUS_SAFETY_REJECTED;
        snprintf(res.slot_name, sizeof(res.slot_name), "input");
        snprintf(res.reason, sizeof(res.reason), "%s", norm.safety_reason);
        res.command = cmd_make_unknown(norm.safety_reason);
        return res;
    }

    /* 2. Safety filter */
    safety_check_t safety = extractor_check_safety(norm.normalized);
    if (safety.safety != SAFETY_PASS) {
        res.status = SLOT_STATUS_SAFETY_REJECTED;
        snprintf(res.slot_name, sizeof(res.slot_name), "safety");
        snprintf(res.reason, sizeof(res.reason), "%s", safety.reason);
        res.command = cmd_make_unknown(safety.reason);
        return res;
    }

    const char *text = norm.normalized;

    /* 3. Find Pins: check for gpio<N> tokens */
    uint8_t found_pins[8];
    size_t pin_count = 0;
    bool pin_overflow = false;

    char text_copy[sizeof(norm.normalized)];
    memcpy(text_copy, text, sizeof(text_copy));
    char *saveptr = NULL;
    char *tok = strtok_r(text_copy, " ", &saveptr);
    while (tok != NULL) {
        if (strncmp(tok, "gpio", 4) == 0 && isdigit((unsigned char)tok[4])) {
            char *endp = NULL;
            unsigned long pval = strtoul(tok + 4, &endp, 10);
            if (endp != tok + 4 && *endp == '\0') {
                if (pval >= 40) {
                    pin_overflow = true;
                }
                if (pin_count < 8) {
                    found_pins[pin_count++] = (uint8_t)pval;
                }
            }
        }
        tok = strtok_r(NULL, " ", &saveptr);
    }

    if (pin_overflow) {
        res.status = SLOT_STATUS_INVALID;
        snprintf(res.slot_name, sizeof(res.slot_name), "pin");
        snprintf(res.reason, sizeof(res.reason), "Pin number out of valid range (GPIO 0-39)");
        res.command = cmd_make_unknown("invalid pin");
        return res;
    }

    /* 4. Determine Intent */
    bool is_blink = has_exact_token(text, "blink") || has_phrase_str(text, "flash");
    bool is_pulse = has_exact_token(text, "pulse");
    bool is_pwm = has_exact_token(text, "pwm") || has_exact_token(text, "duty") ||
                  has_exact_token(text, "frequency") || has_phrase_str(text, "dim");
    bool is_toggle = has_exact_token(text, "toggle");
    bool is_adc = has_exact_token(text, "adc") || has_exact_token(text, "analog") ||
                  has_phrase_str(text, "voltage");
    bool is_read = has_exact_token(text, "read") || has_phrase_str(text, "read pin");
    bool is_i2c = has_exact_token(text, "i2c") && has_exact_token(text, "scan");
    bool is_uart = has_exact_token(text, "uart") && (has_exact_token(text, "send") || has_exact_token(text, "tx"));
    bool is_status = has_exact_token(text, "status");
    bool is_help = has_exact_token(text, "help");

    /* Level determination */
    bool has_high = has_exact_token(text, "high") || has_exact_token(text, "on") ||
                    has_exact_token(text, "1") || has_phrase_str(text, "turn on");
    bool has_low = has_exact_token(text, "low") || has_exact_token(text, "off") ||
                   has_exact_token(text, "0") || has_phrase_str(text, "turn off");

    /* Level conflict check */
    if (has_high && has_low && !is_toggle && !is_blink) {
        res.status = SLOT_STATUS_INVALID;
        snprintf(res.slot_name, sizeof(res.slot_name), "level");
        snprintf(res.reason, sizeof(res.reason), "Conflicting levels: both HIGH/ON and LOW/OFF specified");
        res.command = cmd_make_unknown("conflicting levels");
        return res;
    }

    /* Pin conflict check */
    if (pin_count > 1) {
        res.status = SLOT_STATUS_AMBIGUOUS;
        snprintf(res.slot_name, sizeof(res.slot_name), "pin");
        snprintf(res.reason, sizeof(res.reason), "Multiple pins specified (ambiguous single-command target)");
        res.command = cmd_make_unknown("ambiguous pins");
        return res;
    }

    /* Intent: GPIO_BLINK */
    if (is_blink) {
        if (pin_count == 0) {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "pin");
            snprintf(res.reason, sizeof(res.reason), "Missing required pin for blink");
            return res;
        }
        /* Look for duration */
        uint32_t on_ms = 0;
        uint32_t off_ms = 0;
        uint32_t repeats = 1;

        memcpy(text_copy, text, sizeof(text_copy));
        tok = strtok_r(text_copy, " ", &saveptr);
        while (tok != NULL) {
            if (isdigit((unsigned char)tok[0])) {
                char *endp = NULL;
                uint64_t v = strtoull(tok, &endp, 10);
                if (endp != tok && *endp == '\0') {
                    char *next = strtok_r(NULL, " ", &saveptr);
                    if (next != NULL && strcmp(next, "s") == 0) {
                        v *= 1000ULL;
                    }
                    if (v == 0 || v > EXTRACTOR_MAX_DURATION_MS) {
                        res.status = SLOT_STATUS_INVALID;
                        snprintf(res.slot_name, sizeof(res.slot_name), "duration");
                        snprintf(res.reason, sizeof(res.reason), "Duration %llu ms out of range (1 - %u ms)",
                                 (unsigned long long)v, (unsigned)EXTRACTOR_MAX_DURATION_MS);
                        return res;
                    }
                    if (on_ms == 0) on_ms = (uint32_t)v;
                    else if (off_ms == 0) off_ms = (uint32_t)v;
                }
            }
            tok = strtok_r(NULL, " ", &saveptr);
        }
        if (on_ms == 0) {
            on_ms = 1000; /* Default 1000ms if not specified */
        }
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_gpio_blink(found_pins[0], on_ms, off_ms ? off_ms : on_ms, repeats);
        return res;
    }

    /* Intent: GPIO_PULSE */
    if (is_pulse) {
        if (pin_count == 0) {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "pin");
            snprintf(res.reason, sizeof(res.reason), "Missing required pin for pulse");
            return res;
        }
        uint32_t duration_ms = 0;
        memcpy(text_copy, text, sizeof(text_copy));
        tok = strtok_r(text_copy, " ", &saveptr);
        while (tok != NULL) {
            if (isdigit((unsigned char)tok[0])) {
                char *endp = NULL;
                uint64_t v = strtoull(tok, &endp, 10);
                if (endp != tok && *endp == '\0') {
                    char *next = strtok_r(NULL, " ", &saveptr);
                    if (next != NULL && strcmp(next, "s") == 0) {
                        v *= 1000ULL;
                    }
                    if (v == 0 || v > EXTRACTOR_MAX_DURATION_MS) {
                        res.status = SLOT_STATUS_INVALID;
                        snprintf(res.slot_name, sizeof(res.slot_name), "duration");
                        snprintf(res.reason, sizeof(res.reason), "Pulse duration %llu ms out of range",
                                 (unsigned long long)v);
                        return res;
                    }
                    duration_ms = (uint32_t)v;
                    break;
                }
            }
            tok = strtok_r(NULL, " ", &saveptr);
        }
        if (duration_ms == 0) {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "duration");
            snprintf(res.reason, sizeof(res.reason), "Missing duration for pulse");
            return res;
        }
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_gpio_pulse(found_pins[0], duration_ms);
        return res;
    }

    /* Intent: PWM */
    if (is_pwm) {
        if (pin_count == 0) {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "pin");
            snprintf(res.reason, sizeof(res.reason), "Missing required pin for PWM");
            return res;
        }
        uint32_t freq_hz = 0;
        uint8_t duty_pct = 0;
        bool found_duty = false;

        memcpy(text_copy, text, sizeof(text_copy));
        tok = strtok_r(text_copy, " ", &saveptr);
        while (tok != NULL) {
            if (isdigit((unsigned char)tok[0])) {
                char *endp = NULL;
                uint64_t v = strtoull(tok, &endp, 10);
                if (endp != tok && *endp == '\0') {
                    char *next = strtok_r(NULL, " ", &saveptr);
                    if (next != NULL) {
                        if (strcmp(next, "hz") == 0) {
                            freq_hz = (uint32_t)v;
                        } else if (strcmp(next, "khz") == 0) {
                            v *= 1000ULL;
                            freq_hz = (uint32_t)v;
                        } else if (strcmp(next, "mhz") == 0) {
                            v *= 1000000ULL;
                            freq_hz = (uint32_t)v;
                        } else if (strcmp(next, "%") == 0) {
                            if (v > 100) {
                                res.status = SLOT_STATUS_INVALID;
                                snprintf(res.slot_name, sizeof(res.slot_name), "duty");
                                snprintf(res.reason, sizeof(res.reason), "Duty %llu%% exceeds 100%%", (unsigned long long)v);
                                return res;
                            }
                            duty_pct = (uint8_t)v;
                            found_duty = true;
                        }
                    }
                }
            }
            tok = strtok_r(NULL, " ", &saveptr);
        }

        if (freq_hz != 0 && (freq_hz < EXTRACTOR_MIN_LEDC_FREQ || freq_hz > EXTRACTOR_MAX_LEDC_FREQ)) {
            res.status = SLOT_STATUS_INVALID;
            snprintf(res.slot_name, sizeof(res.slot_name), "frequency");
            snprintf(res.reason, sizeof(res.reason), "Frequency outside LEDC range 1Hz-40MHz");
            return res;
        }

        if (!found_duty && freq_hz == 0) {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "frequency_or_duty");
            snprintf(res.reason, sizeof(res.reason), "PWM requires frequency or duty cycle");
            return res;
        }

        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_pwm(found_pins[0], freq_hz ? freq_hz : 5000, found_duty ? duty_pct : 50);
        return res;
    }

    /* Intent: GPIO_TOGGLE */
    if (is_toggle) {
        if (pin_count == 0) {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "pin");
            snprintf(res.reason, sizeof(res.reason), "Missing required pin for toggle");
            return res;
        }
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_gpio_toggle(found_pins[0]);
        return res;
    }

    /* Intent: ADC_READ */
    if (is_adc) {
        if (pin_count == 0) {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "pin");
            snprintf(res.reason, sizeof(res.reason), "Missing pin for ADC read");
            return res;
        }
        bool raw = has_exact_token(text, "raw");
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_adc_read(found_pins[0], raw);
        return res;
    }

    /* Intent: GPIO_READ */
    if (is_read) {
        if (pin_count == 0) {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "pin");
            snprintf(res.reason, sizeof(res.reason), "Missing pin for read");
            return res;
        }
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_gpio_read(found_pins[0]);
        return res;
    }

    /* Intent: I2C_SCAN */
    if (is_i2c) {
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_i2c_scan(0);
        return res;
    }

    /* Intent: UART_SEND */
    if (is_uart) {
        uint8_t uart_id = 1; /* Default UART1 */
        if (has_exact_token(text, "uart0") || has_exact_token(text, "uart 0")) uart_id = 0;
        else if (has_exact_token(text, "uart2") || has_exact_token(text, "uart 2")) uart_id = 2;

        /* Look for quoted string payload in original input */
        const char *q1 = strchr(input, '"');
        if (q1 == NULL) q1 = strchr(input, '\'');

        char payload[CMD_UART_PAYLOAD_MAX] = {0};
        if (q1 != NULL) {
            char quote_char = *q1;
            const char *q2 = strchr(q1 + 1, quote_char);
            if (q2 != NULL) {
                size_t qlen = (size_t)(q2 - (q1 + 1));
                if (qlen >= sizeof(payload)) qlen = sizeof(payload) - 1;
                memcpy(payload, q1 + 1, qlen);
                payload[qlen] = '\0';
            }
        }
        if (payload[0] == '\0') {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "payload");
            snprintf(res.reason, sizeof(res.reason), "Missing quoted payload for UART send");
            return res;
        }
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_uart_send(uart_id, 115200, payload, strlen(payload));
        return res;
    }

    /* Intent: GET_STATUS */
    if (is_status) {
        int16_t pin = (pin_count > 0) ? (int16_t)found_pins[0] : -1;
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_get_status(pin);
        return res;
    }

    /* Intent: HELP */
    if (is_help) {
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_help("");
        return res;
    }

    /* Intent: GPIO_WRITE (e.g. "turn on pin 2", "set gpio4 high", "gpio2 low") */
    if (has_high || has_low) {
        if (pin_count == 0) {
            res.status = SLOT_STATUS_MISSING;
            snprintf(res.slot_name, sizeof(res.slot_name), "pin");
            snprintf(res.reason, sizeof(res.reason), "Missing pin target for write command");
            return res;
        }
        uint8_t level = has_high ? 1 : 0;
        res.status = SLOT_STATUS_OK;
        res.command = cmd_make_gpio_write(found_pins[0], level);
        return res;
    }

    /* Unknown / Ambiguous */
    res.status = SLOT_STATUS_AMBIGUOUS;
    snprintf(res.slot_name, sizeof(res.slot_name), "intent");
    snprintf(res.reason, sizeof(res.reason), "No actionable command intent found");
    res.command = cmd_make_unknown("unrecognized intent");
    return res;
}

const char *safety_result_name(safety_result_t safety)
{
    switch (safety) {
    case SAFETY_PASS: return "PASS";
    case SAFETY_REJECT_EMPTY: return "REJECT_EMPTY";
    case SAFETY_REJECT_TOO_LONG: return "REJECT_TOO_LONG";
    case SAFETY_REJECT_NON_ASCII: return "REJECT_NON_ASCII";
    case SAFETY_REJECT_NEGATION: return "REJECT_NEGATION";
    case SAFETY_REJECT_QUESTION: return "REJECT_QUESTION";
    case SAFETY_REJECT_INJECTION: return "REJECT_INJECTION";
    case SAFETY_REJECT_MULTI_COMMAND: return "REJECT_MULTI_COMMAND";
    case SAFETY_REJECT_CONFIGURATION: return "REJECT_CONFIGURATION";
    default: return "UNKNOWN";
    }
}

const char *slot_status_name(slot_status_t status)
{
    switch (status) {
    case SLOT_STATUS_OK: return "OK";
    case SLOT_STATUS_MISSING: return "MISSING";
    case SLOT_STATUS_INVALID: return "INVALID";
    case SLOT_STATUS_AMBIGUOUS: return "AMBIGUOUS";
    case SLOT_STATUS_SAFETY_REJECTED: return "SAFETY_REJECTED";
    default: return "UNKNOWN";
    }
}
