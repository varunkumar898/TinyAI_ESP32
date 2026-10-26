#include "SerialConsole.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Automation.h"
#include "CommandEngine.h"
#include "PinManager.h"
#include "SensorManager.h"
#include "board_profile.h"
#include "config_store.h"

#ifdef ESP_PLATFORM
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
static TaskHandle_t s_console_task_handle = NULL;
#endif

#define CONSOLE_LINE_MAX 256
#define CONSOLE_MAX_ARGS 16
#define CONSOLE_PENDING_TIMEOUT_MS 60000u
#define CONSOLE_TASK_STACK 6144
#define CONSOLE_OUT_BUF_MAX 2048

typedef enum {
    CONSOLE_PENDING_NONE = 0,
    CONSOLE_PENDING_ALL_ON,
    CONSOLE_PENDING_GROUP_ON,
} console_pending_kind_t;

typedef struct {
    console_pending_kind_t kind;
    char group[PM_GROUP_NAME_MAX];
    uint32_t created_ms;
} console_pending_t;

static console_pending_t s_pending;
static char s_console_out_buffer[CONSOLE_OUT_BUF_MAX];

static void outf(serial_console_output_fn out, void *context, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(s_console_out_buffer, sizeof(s_console_out_buffer), format, args);
    va_end(args);
    if (written < 0) {
        s_console_out_buffer[0] = '\0';
    } else if ((size_t)written >= sizeof(s_console_out_buffer)) {
        s_console_out_buffer[sizeof(s_console_out_buffer) - 1] = '\0';
    }
    if (out != NULL) {
        out(context, s_console_out_buffer);
    } else {
        fputs(s_console_out_buffer, stdout);
        fflush(stdout);
    }
}

static uint32_t console_now_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000);
#else
    return 0;
#endif
}

static char *skip_ws(char *text)
{
    while (text != NULL && *text != '\0' && isspace((unsigned char)*text)) {
        ++text;
    }
    return text;
}

/* Tokenizer with double-quoted phrases. Returns (size_t)-1 on an unclosed
 * quote. The input buffer is modified in place. */
static size_t tokenize(char *line, char **argv, size_t max_args)
{
    size_t count = 0;
    char *p = line;
    while (true) {
        p = skip_ws(p);
        if (*p == '\0') {
            break;
        }
        if (count >= max_args) {
            return (size_t)-1;
        }
        if (*p == '"') {
            ++p;
            argv[count++] = p;
            while (*p != '\0' && *p != '"') {
                ++p;
            }
            if (*p != '"') {
                return (size_t)-1;
            }
            *p++ = '\0';
        } else {
            argv[count++] = p;
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                ++p;
            }
            if (*p != '\0') {
                *p++ = '\0';
            }
        }
    }
    return count;
}

static bool parse_u32(const char *text, unsigned min, unsigned max, uint32_t *out)
{
    if (text == NULL || *text == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == NULL || *end != '\0' || value < min || value > max) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool parse_i32(const char *text, int32_t *out)
{
    if (text == NULL || *text == '\0') {
        return false;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

static bool parse_on_off(const char *text, bool *out)
{
    if (text == NULL) {
        return false;
    }
    if (strcmp(text, "on") == 0 || strcmp(text, "true") == 0 ||
        strcmp(text, "yes") == 0 || strcmp(text, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "false") == 0 ||
        strcmp(text, "no") == 0 || strcmp(text, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool all_digits(const char *text)
{
    if (text == NULL || *text == '\0') {
        return false;
    }
    for (const char *p = text; *p != '\0'; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static bool resolve_target_pin(const char *text, uint8_t *pin)
{
    if (text == NULL || pin == NULL) {
        return false;
    }
    if (all_digits(text)) {
        uint32_t value = 0;
        if (!parse_u32(text, 0, PM_MAX_PINS - 1, &value)) {
            return false;
        }
        *pin = (uint8_t)value;
        return true;
    }
    return pin_manager_resolve_name(text, pin);
}

static const char *display_name(const pm_pin_status_t *status, char *buffer,
                                size_t size)
{
    if (status->device_name[0] != '\0') {
        return status->device_name;
    }
    snprintf(buffer, size, "GPIO%u", (unsigned)status->pin);
    return buffer;
}

static void print_pin_detail(pm_pin_status_t *status, serial_console_output_fn out,
                             void *context)
{
    char fallback[16];
    const char *name = display_name(status, fallback, sizeof(fallback));
    if (!status->initialized) {
        outf(out, context, "%s (GPIO%u): not initialized%s\n", name,
             (unsigned)status->pin,
             status->reserved ? " [reserved]" : "");
        return;
    }
    outf(out, context,
         "%s (GPIO%u): %s, %s, pull=%s, %s%s%s%s%s%s\n", name,
         (unsigned)status->pin,
         status->mode == GPIO_MODE_OUTPUT ? "output" : "input",
         status->logical_state ? "ON" : "OFF", GPIO_PullName(status->pull),
         status->active_high ? "active-high" : "active-low",
         status->ai_allowed ? ", AI-allowed" : ", local-only",
         status->test_approved ? ", test-approved" : "",
         status->locked ? ", LOCKED" : "",
         status->configured ? "" : ", unnamed",
         status->reserved ? ", RESERVED" : "");
    if (status->alias_count > 0) {
        outf(out, context, "  aliases:");
        for (uint8_t i = 0; i < status->alias_count; ++i) {
            outf(out, context, " %s", status->aliases[i]);
        }
        outf(out, context, "\n");
    }
}

static void print_sensor_line(const sensor_sample_t *sensor,
                              serial_console_output_fn out, void *context)
{
    if (sensor->kind == SENSOR_KIND_ADC1) {
        outf(out, context, "  %-16s adc1 GPIO%-2u %s raw=%ld approx=%ldmV age=%lums\n",
             sensor->name, (unsigned)sensor->pin,
             sensor->valid ? "valid" : "INVALID", (long)sensor->raw,
             (long)sensor->value, (unsigned long)sensor->last_sample_ms);
    } else {
        outf(out, context, "  %-16s digital GPIO%-2u %s value=%s age=%lums\n",
             sensor->name, (unsigned)sensor->pin,
             sensor->valid ? "valid" : "INVALID",
             sensor->value ? "HIGH" : "LOW",
             (unsigned long)sensor->last_sample_ms);
    }
}

static void usage_assign(serial_console_output_fn out, void *context)
{
    outf(out, context,
         "Usage: assign <gpio> <name> [active-low|active-high] [ai|local] "
         "[test|no-test]\n");
}

static void print_help(serial_console_output_fn out, void *context)
{
    outf(out, context,
         "TinyAI ESP32 controller console (local management + optional TinyML)\n"
         "Pins:      pins [safe] | pin list | pin status all | pin <gpio> status\n"
         "           pin init <gpio> <input|output> [pullup|pulldown]\n"
         "           pin <gpio> on|off|toggle|read|deinit\n"
         "           pin assign <gpio> <name> [active-low|active-high] [ai|local]\n"
         "           deinit <gpio> | assign <gpio> <name> [flags] | alias <gpio> <alias>\n"
         "           release <gpio> | lock <gpio> on|off | test <gpio> on|off\n"
         "Devices:   devices | status [name|gpio] | log [n] | log clear\n"
         "Groups:    group create|add|remove|list|show|on|off ...\n"
         "Outputs:   all on | all off | outputs all on|off | group on|off <name>\n"
         "AI:        ai on|off | ai status | ai allow <name|gpio> on|off | ai model\n"
         "           ai <natural language> | ai turn on led one | ai light status\n"
         "Natural:   do <text> | classify <text> | confirm | cancel | bench [n]\n"
         "Sensors:   sensor add <name> <digital|adc1> <gpio> | sensor remove <name>\n"
         "           sensors | sensor read <name|all>\n"
         "Automation: sensor pir <in> [out|led1] [high|low] [hold_ms]\n"
         "           sensor button <in> <out|led1> [high|low]\n"
         "           rule add <name> <sensor> above|below <threshold> <hyst> <out>\n"
         "           rule remove|enable|disable|reset <name> | rules\n"
         "           sensor status | sensor clear | automation on|off|status|clear\n"
         "Storage:   save | load | config status | config erase\n"
         "System:    board | heap | model | reboot | help\n"
         "Safety:    AI/remote writes require configured, AI-allowed devices.\n"
         "           Multi-output ON needs local 'confirm'. Negations, questions,\n"
         "           prompt injection and pin configuration text never actuate.\n");
}

static void console_pending_clear(void)
{
    memset(&s_pending, 0, sizeof(s_pending));
}

static bool console_pending_active(void)
{
    return s_pending.kind != CONSOLE_PENDING_NONE;
}

static void console_pending_expire_if_needed(serial_console_output_fn out,
                                             void *context)
{
    if (s_pending.kind == CONSOLE_PENDING_NONE) {
        return;
    }
    uint32_t age = console_now_ms() - s_pending.created_ms;
    if (age > CONSOLE_PENDING_TIMEOUT_MS) {
        console_pending_clear();
        outf(out, context, "Pending confirmation expired. Reissue the command.\n");
    }
}

static void command_ai(serial_console_output_fn out, void *context, size_t argc,
                       char **argv)
{
    if (argc <= 1 || strcmp(argv[1], "status") == 0) {
        outf(out, context,
             "TinyAI: compiled=%s enabled=%s model=%s\n",
             command_engine_ai_available() ? "yes" : "no",
             command_engine_ai_enabled() ? "yes" : "no",
             command_engine_model_available() ? "available" : "unavailable");
        return;
    }
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
        if (!command_engine_ai_available()) {
            outf(out, context, "TinyAI is not compiled into this firmware.\n");
            return;
        }
        bool enabled = strcmp(argv[1], "on") == 0;
        command_engine_set_ai_enabled(enabled);
        outf(out, context, "TinyAI natural-language control %s.\n",
             enabled ? "enabled" : "disabled");
        return;
    }
    if (strcmp(argv[1], "allow") == 0) {
        if (argc != 4) {
            outf(out, context, "Usage: ai allow <name|gpio> on|off\n");
            return;
        }
        uint8_t pin = 0;
        bool allowed = false;
        if (!resolve_target_pin(argv[2], &pin)) {
            outf(out, context, "Unknown target '%s'.\n", argv[2]);
            return;
        }
        if (!parse_on_off(argv[3], &allowed)) {
            outf(out, context, "Expected on or off.\n");
            return;
        }
        pm_status_t status = pin_manager_set_ai_allowed(pin, allowed);
        outf(out, context, "AI permission for GPIO%u: %s (%s).\n", (unsigned)pin,
             allowed ? "allowed" : "denied", pin_manager_status_name(status));
        return;
    }
    if (strcmp(argv[1], "model") == 0) {
        char info[384];
        command_engine_model_info(info, sizeof(info));
        outf(out, context, "%s\n", info);
        return;
    }
    outf(out, context, "Usage: ai [status|on|off|allow <target> on|off|model]\n");
}

static void command_board(serial_console_output_fn out, void *context)
{
    board_runtime_info_t info;
    board_profile_get_runtime_info(&info);
    outf(out, context,
         "Board: %s\nModule: %s\nFlash: %u bytes, PSRAM: %s\n"
         "Chip: %u core(s), revision 0x%04x\n"
         "Heap: free=%u min=%u largest=%u\n",
         info.board_name, info.module_name, (unsigned)info.flash_bytes,
         info.psram_present ? "yes" : "no", (unsigned)info.chip_cores,
         (unsigned)info.chip_revision, (unsigned)info.free_heap_bytes,
         (unsigned)info.minimum_free_heap_bytes,
         (unsigned)info.largest_free_block_bytes);
}

static void command_pins(serial_console_output_fn out, void *context, size_t argc,
                         char **argv)
{
    bool safe_only = argc >= 2 && strcmp(argv[1], "safe") == 0;
    unsigned shown = 0;
    for (uint8_t pin = 0; pin < BOARD_PROFILE_PIN_COUNT; ++pin) {
        board_pin_caps_t caps;
        if (!board_profile_get_pin_caps(pin, &caps) || !caps.present) {
            continue;
        }
        if (safe_only && caps.reserved) {
            continue;
        }
        char line[96];
        board_profile_format_pin(pin, line, sizeof(line));
        pm_pin_status_t status;
        if (pin_manager_get_status(pin, &status) == PM_OK) {
            if (status.configured) {
                outf(out, context, "%s [%s]%s\n", line, status.device_name,
                     status.locked ? " LOCKED" : "");
            } else if (status.initialized) {
                outf(out, context, "%s [initialized]\n", line);
            } else {
                outf(out, context, "%s\n", line);
            }
        } else {
            outf(out, context, "%s\n", line);
        }
        ++shown;
    }
    outf(out, context, "%u pin(s) shown.\n", shown);
}

static void command_devices(serial_console_output_fn out, void *context)
{
    pm_pin_status_t pins[PM_MAX_PINS];
    size_t count = pin_manager_list(pins, PM_MAX_PINS, true);
    size_t shown = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!pins[i].configured) {
            continue;
        }
        ++shown;
        outf(out, context, "  %-16s GPIO%-2u %-6s %s%s%s\n", pins[i].device_name,
             (unsigned)pins[i].pin,
             pins[i].mode == GPIO_MODE_OUTPUT ? "output" : "input",
             pins[i].logical_state ? "ON" : "OFF",
             pins[i].ai_allowed ? " ai" : " local",
             pins[i].test_approved ? " test" : "");
    }
    if (shown == 0) {
        outf(out, context,
             "No devices configured. Example:\n"
             "  pin 23 output\n  assign 23 led1 ai test\n  save\n");
    }
}

static void command_status(serial_console_output_fn out, void *context, size_t argc,
                           char **argv)
{
    pm_pin_status_t pins[PM_MAX_PINS];
    if (argc >= 2) {
        uint8_t pin = 0;
        if (!resolve_target_pin(argv[1], &pin)) {
            outf(out, context, "Unknown target '%s'.\n", argv[1]);
            return;
        }
        if (pin_manager_get_status(pin, &pins[0]) != PM_OK) {
            outf(out, context, "Invalid GPIO%u.\n", (unsigned)pin);
            return;
        }
        print_pin_detail(&pins[0], out, context);
        return;
    }
    size_t count = pin_manager_list(pins, PM_MAX_PINS, true);
    size_t shown = 0;
    for (size_t i = 0; i < count; ++i) {
        if (pins[i].configured) {
            print_pin_detail(&pins[i], out, context);
            ++shown;
        }
    }
    if (shown == 0) {
        outf(out, context, "No configured devices.\n");
    }
}

static void console_blink_notify(void *context, const char *msg)
{
    (void)context;
    fputs(msg, stdout);
    fflush(stdout);
}

static void command_blink(serial_console_output_fn out, void *context, size_t argc,
                          char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "cancel") == 0) {
        (void)pin_manager_blink_cancel(out, context);
        return;
    }
    if (argc < 3) {
        outf(out, context,
             "Usage: gpio blink <gpio> <duration_ms> | gpio blink cancel\n");
        return;
    }

    uint32_t pin_value = 0;
    if (!parse_u32(argv[1], 0, PM_MAX_PINS - 1, &pin_value)) {
        outf(out, context,
             "Target: invalid | Duration: %s | Validation: rejected (GPIO must be 0..39)\n",
             argv[2]);
        return;
    }
    uint8_t pin = (uint8_t)pin_value;

    uint32_t duration_ms = 0;
    if (!parse_u32(argv[2], 1, 60000, &duration_ms)) {
        outf(out, context,
             "Target: GPIO%u | Duration: %s | Validation: rejected (duration must be 1..60000 ms)\n",
             (unsigned)pin, argv[2]);
        return;
    }

    if (pin == 2 && !board_profile_is_gpio2_authorized()) {
        outf(out, context,
             "Target: GPIO2 | Duration: %lu ms | Validation: denied (strapping pin GPIO2 requires session authorization: run 'gpio authorize 2')\n",
             (unsigned long)duration_ms);
        return;
    }

    outf(out, context,
         "Target: GPIO%u | Duration: %lu ms | Validation: approved\nExecution: started\n",
         (unsigned)pin, (unsigned long)duration_ms);

    pm_blink_notify_fn notify = out != NULL ? (pm_blink_notify_fn)out : console_blink_notify;
    void *notify_ctx = out != NULL ? context : NULL;
    pm_status_t status = pin_manager_blink_start(pin, duration_ms, notify, notify_ctx);
    if (status != PM_OK) {
        outf(out, context, "Execution: failed (%s)\n", pin_manager_status_name(status));
    }
}

static void command_authorize(serial_console_output_fn out, void *context, size_t argc,
                             char **argv)
{
    if (argc < 2) {
        outf(out, context, "Usage: gpio authorize <gpio>\n");
        return;
    }
    uint32_t pin = 0;
    if (!parse_u32(argv[1], 0, PM_MAX_PINS - 1, &pin)) {
        outf(out, context, "GPIO must be 0..39.\n");
        return;
    }
    if (pin == 2) {
        board_profile_set_gpio2_authorized(true);
        outf(out, context,
             "GPIO2 session authorization granted (session-only; will not persist across reboot).\n");
        return;
    }
    if (pin == 0 || pin == 5 || pin == 12 || pin == 15) {
        outf(out, context,
             "Cannot authorize GPIO%u: protected strapping pin. Only GPIO2 is supported for session authorization.\n",
             (unsigned)pin);
        return;
    }
    if (pin >= 6 && pin <= 11) {
        outf(out, context, "Cannot authorize GPIO%u: flash pins are hard-blocked.\n",
             (unsigned)pin);
        return;
    }
    if (pin == 1 || pin == 3) {
        outf(out, context, "Cannot authorize GPIO%u: UART0 console pins are hard-blocked.\n",
             (unsigned)pin);
        return;
    }
    outf(out, context, "GPIO%u does not require strapping authorization.\n", (unsigned)pin);
}

static void command_revoke(serial_console_output_fn out, void *context, size_t argc,
                           char **argv)
{
    if (argc < 2) {
        outf(out, context, "Usage: gpio revoke <gpio>\n");
        return;
    }
    uint32_t pin = 0;
    if (!parse_u32(argv[1], 0, PM_MAX_PINS - 1, &pin)) {
        outf(out, context, "GPIO must be 0..39.\n");
        return;
    }
    if (pin == 2) {
        board_profile_set_gpio2_authorized(false);
        uint8_t active_pin = 0;
        if (pin_manager_blink_is_active(&active_pin) && active_pin == 2) {
            (void)pin_manager_blink_cancel(out, context);
        }
        outf(out, context, "GPIO2 session authorization revoked.\n");
        return;
    }
    outf(out, context, "GPIO%u is not session-authorized.\n", (unsigned)pin);
}

static void command_assign(serial_console_output_fn out, void *context, size_t argc,
                           char **argv);

static void command_gpio(serial_console_output_fn out, void *context, size_t argc,
                         char **argv);

static void command_pin(serial_console_output_fn out, void *context, size_t argc,
                        char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "blink") == 0) {
        command_blink(out, context, argc - 1, argv + 1);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "authorize") == 0) {
        command_authorize(out, context, argc - 1, argv + 1);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "revoke") == 0) {
        command_revoke(out, context, argc - 1, argv + 1);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        command_devices(out, context);
        return;
    }
    if (argc >= 4 && strcmp(argv[1], "status") == 0 &&
        strcmp(argv[2], "all") == 0) {
        command_status(out, context, 0, NULL);
        return;
    }
    if (argc >= 3 && strcmp(argv[1], "assign") == 0) {
        command_assign(out, context, argc - 1, argv + 1);
        return;
    }

    size_t index = 1;
    if (argc >= 2 && strcmp(argv[1], "init") == 0) {
        index = 2;
    }
    if (index >= argc) {
        outf(out, context,
             "Usage: pin list | pin status all | pin <gpio> status\n"
             "       pin init <gpio> <input|output> [pullup|pulldown]\n"
             "       pin <gpio> on|off|toggle|read|deinit\n"
             "       pin assign <gpio> <name> [flags]\n");
        return;
    }

    uint32_t pin_value = 0;
    if (!parse_u32(argv[index], 0, PM_MAX_PINS - 1, &pin_value)) {
        outf(out, context, "GPIO must be 0..39.\n");
        return;
    }
    uint8_t pin = (uint8_t)pin_value;
    if (index + 1 >= argc) {
        outf(out, context, "Missing pin action or mode.\n");
        return;
    }
    const char *action = argv[index + 1];

    if (strcmp(action, "status") == 0) {
        pm_pin_status_t status;
        if (pin_manager_get_status(pin, &status) != PM_OK) {
            outf(out, context, "Invalid GPIO%u.\n", (unsigned)pin);
            return;
        }
        print_pin_detail(&status, out, context);
        return;
    }
    if (strcmp(action, "read") == 0) {
        bool physical = false;
        bool logical = false;
        pm_status_t status = pin_manager_read(pin, &physical, &logical);
        if (status != PM_OK) {
            outf(out, context, "GPIO%u read: %s.\n", (unsigned)pin,
                 pin_manager_status_name(status));
            return;
        }
        outf(out, context,
             "GPIO%u read: physical=%s logical=%s.\n", (unsigned)pin,
             physical ? "HIGH" : "LOW", logical ? "ON" : "OFF");
        return;
    }
    if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0) {
        bool on = strcmp(action, "on") == 0;
        pm_status_t status =
            pin_manager_output(pin, on, PM_ORIGIN_LOCAL,
                               on ? "PIN_ON" : "PIN_OFF", 1.0f, "console");
        outf(out, context, "GPIO%u %s: %s.\n", (unsigned)pin,
             on ? "ON" : "OFF", pin_manager_status_name(status));
        return;
    }
    if (strcmp(action, "toggle") == 0) {
        pm_status_t status =
            pin_manager_toggle(pin, PM_ORIGIN_LOCAL, "PIN_TOGGLE", 1.0f,
                               "console");
        outf(out, context, "GPIO%u toggle: %s.\n", (unsigned)pin,
             pin_manager_status_name(status));
        return;
    }
    if (strcmp(action, "deinit") == 0) {
        pm_status_t status = pin_manager_pin_deinit(pin);
        outf(out, context, "GPIO%u deinit: %s.\n", (unsigned)pin,
             pin_manager_status_name(status));
        return;
    }

    GPIO_Mode mode;
    GPIO_Pull pull = GPIO_PULL_NONE;
    if (strcmp(action, "input") == 0 || strcmp(action, "in") == 0) {
        mode = GPIO_MODE_INPUT;
    } else if (strcmp(action, "output") == 0 ||
               strcmp(action, "out") == 0) {
        mode = GPIO_MODE_OUTPUT;
    } else if (strcmp(action, "input-pullup") == 0 ||
               strcmp(action, "pullup") == 0) {
        mode = GPIO_MODE_INPUT;
        pull = GPIO_PULL_UP;
    } else if (strcmp(action, "input-pulldown") == 0 ||
               strcmp(action, "pulldown") == 0) {
        mode = GPIO_MODE_INPUT;
        pull = GPIO_PULL_DOWN;
    } else {
        outf(out, context, "Unknown pin action or mode '%s'.\n", action);
        return;
    }
    if (index + 2 < argc) {
        if (strcmp(argv[index + 2], "pullup") == 0 ||
            strcmp(argv[index + 2], "up") == 0) {
            pull = GPIO_PULL_UP;
        } else if (strcmp(argv[index + 2], "pulldown") == 0 ||
                   strcmp(argv[index + 2], "down") == 0) {
            pull = GPIO_PULL_DOWN;
        } else {
            outf(out, context, "Unknown pull option '%s'.\n",
                 argv[index + 2]);
            return;
        }
    }
    pm_status_t status = pin_manager_pin_init(pin, mode, pull);
    outf(out, context, "GPIO%u init %s: %s.\n", (unsigned)pin,
         GPIO_ModeName(mode), pin_manager_status_name(status));
}

static void command_gpio(serial_console_output_fn out, void *context, size_t argc,
                         char **argv)
{
    if (argc < 2) {
        outf(out, context,
             "Usage: gpio blink <gpio> <duration_ms> | gpio blink cancel\n"
             "       gpio authorize <gpio> | gpio revoke <gpio>\n"
             "       gpio <gpio> on|off|toggle|read\n");
        return;
    }
    if (strcmp(argv[1], "blink") == 0) {
        command_blink(out, context, argc - 1, argv + 1);
        return;
    }
    if (strcmp(argv[1], "authorize") == 0) {
        command_authorize(out, context, argc - 1, argv + 1);
        return;
    }
    if (strcmp(argv[1], "revoke") == 0) {
        command_revoke(out, context, argc - 1, argv + 1);
        return;
    }
    command_pin(out, context, argc, argv);
}

static void command_assign(serial_console_output_fn out, void *context, size_t argc,
                           char **argv)
{
    if (argc < 3) {
        usage_assign(out, context);
        return;
    }
    uint32_t pin = 0;
    if (!parse_u32(argv[1], 0, PM_MAX_PINS - 1, &pin)) {
        outf(out, context, "GPIO must be 0..39.\n");
        return;
    }
    bool active_high = true;
    bool ai_allowed = false;
    bool test_approved = false;
    for (size_t i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "active-low") == 0 || strcmp(argv[i], "low") == 0) {
            active_high = false;
        } else if (strcmp(argv[i], "active-high") == 0 ||
                   strcmp(argv[i], "high") == 0) {
            active_high = true;
        } else if (strcmp(argv[i], "ai") == 0) {
            ai_allowed = true;
        } else if (strcmp(argv[i], "local") == 0 ||
                   strcmp(argv[i], "local-only") == 0) {
            ai_allowed = false;
        } else if (strcmp(argv[i], "test") == 0) {
            test_approved = true;
        } else if (strcmp(argv[i], "no-test") == 0) {
            test_approved = false;
        } else {
            outf(out, context, "Unknown assign flag '%s'.\n", argv[i]);
            usage_assign(out, context);
            return;
        }
    }
    pm_status_t status = pin_manager_assign((uint8_t)pin, argv[2], active_high,
                                            ai_allowed, test_approved);
    outf(out, context,
         "Assign GPIO%u as '%s': %s (%s, AI=%s, bulk-test=%s).\n",
         (unsigned)pin, argv[2], pin_manager_status_name(status),
         active_high ? "active-high" : "active-low", ai_allowed ? "yes" : "no",
         test_approved ? "yes" : "no");
}

static void command_group(serial_console_output_fn out, void *context, size_t argc,
                          char **argv)
{
    if (argc < 2) {
        outf(out, context,
             "Usage: group create|add|remove|list|show|on|off ...\n");
        return;
    }
    /* Accept both "group on lights" and the requested "group lights on". */
    if (argc == 3 && (strcmp(argv[2], "on") == 0 ||
                      strcmp(argv[2], "off") == 0)) {
        char *reordered[3] = {"group", argv[2], argv[1]};
        command_group(out, context, 3, reordered);
        return;
    }
    if (strcmp(argv[1], "create") == 0) {
        if (argc != 3) {
            outf(out, context, "Usage: group create <name>\n");
            return;
        }
        pm_status_t status = pin_manager_create_group(argv[2]);
        outf(out, context, "Group '%s' create: %s.\n", argv[2],
             pin_manager_status_name(status));
        return;
    }
    if (strcmp(argv[1], "add") == 0 || strcmp(argv[1], "remove") == 0) {
        if (argc != 4) {
            outf(out, context, "Usage: group %s <name> <gpio>\n", argv[1]);
            return;
        }
        uint32_t pin = 0;
        if (!parse_u32(argv[3], 0, PM_MAX_PINS - 1, &pin)) {
            outf(out, context, "GPIO must be 0..39.\n");
            return;
        }
        pm_status_t status =
            strcmp(argv[1], "add") == 0
                ? pin_manager_add_group_pin(argv[2], (uint8_t)pin)
                : pin_manager_remove_group_pin(argv[2], (uint8_t)pin);
        outf(out, context, "Group '%s' %s GPIO%u: %s.\n", argv[2], argv[1],
             (unsigned)pin, pin_manager_status_name(status));
        return;
    }
    if (strcmp(argv[1], "list") == 0) {
        pm_group_status_t groups[PM_MAX_GROUPS];
        size_t count = pin_manager_list_groups(groups, PM_MAX_GROUPS);
        if (count == 0) {
            outf(out, context, "No groups.\n");
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            outf(out, context, "  %-16s %u output(s)\n", groups[i].name,
                 (unsigned)groups[i].pin_count);
        }
        return;
    }
    if (strcmp(argv[1], "show") == 0) {
        if (argc != 3) {
            outf(out, context, "Usage: group show <name>\n");
            return;
        }
        pm_group_status_t group;
        pm_status_t status = pin_manager_get_group(argv[2], &group);
        if (status != PM_OK) {
            outf(out, context, "Group '%s': %s.\n", argv[2],
                 pin_manager_status_name(status));
            return;
        }
        outf(out, context, "Group '%s' (%u):", group.name,
             (unsigned)group.pin_count);
        for (uint8_t pin = 0; pin < PM_MAX_PINS; ++pin) {
            if ((group.pin_mask & (1ULL << pin)) != 0) {
                outf(out, context, " GPIO%u", (unsigned)pin);
            }
        }
        outf(out, context, "\n");
        return;
    }
    if (strcmp(argv[1], "off") == 0) {
        if (argc != 3) {
            outf(out, context, "Usage: group off <name>\n");
            return;
        }
        size_t affected = 0;
        pm_status_t status = pin_manager_group_apply(
            argv[2], false, true, PM_ORIGIN_LOCAL, "GROUP_OFF", 1.0f, "console",
            &affected);
        outf(out, context, "Group '%s' OFF: %s, %u output(s).\n", argv[2],
             pin_manager_status_name(status), (unsigned)affected);
        return;
    }
    if (strcmp(argv[1], "on") == 0) {
        if (argc != 3) {
            outf(out, context, "Usage: group on <name>\n");
            return;
        }
        pm_group_status_t group;
        pm_status_t status = pin_manager_get_group(argv[2], &group);
        if (status != PM_OK) {
            outf(out, context, "Group '%s': %s.\n", argv[2],
                 pin_manager_status_name(status));
            return;
        }
        memset(&s_pending, 0, sizeof(s_pending));
        s_pending.kind = CONSOLE_PENDING_GROUP_ON;
        snprintf(s_pending.group, sizeof(s_pending.group), "%s", group.name);
        s_pending.created_ms = console_now_ms();
        outf(out, context,
             "Confirmation required: turn %u output(s) in group '%s' ON.\n"
             "Type 'confirm' within %u seconds or 'cancel'.\n",
             (unsigned)group.pin_count, group.name,
             (unsigned)(CONSOLE_PENDING_TIMEOUT_MS / 1000));
        return;
    }
    outf(out, context, "Unknown group action '%s'.\n", argv[1]);
}

static void command_all(serial_console_output_fn out, void *context, bool on)
{
    if (on) {
        memset(&s_pending, 0, sizeof(s_pending));
        s_pending.kind = CONSOLE_PENDING_ALL_ON;
        s_pending.created_ms = console_now_ms();
        outf(out, context,
             "Confirmation required: turn all test-approved outputs ON.\n"
             "Outputs without test approval are excluded.\n"
             "Type 'confirm' within %u seconds or 'cancel'.\n",
             (unsigned)(CONSOLE_PENDING_TIMEOUT_MS / 1000));
        return;
    }
    size_t affected = 0;
    pm_status_t status =
        pin_manager_outputs_all_off(PM_ORIGIN_LOCAL, "console", &affected);
    outf(out, context, "All configured outputs OFF: %s, %u output(s).\n",
         pin_manager_status_name(status), (unsigned)affected);
}

static void print_automation_status(serial_console_output_fn out, void *context)
{
    automation_status_t status;
    automation_get_status(&status);
    outf(out, context, "Automation is %s.\n",
         status.enabled ? "enabled" : "disabled");
    if (status.pir_bound) {
        outf(out, context,
             "  PIR: GPIO%u -> GPIO%u %s-active, hold %u ms, motion=%s, owns-output=%s\n",
             (unsigned)status.pir_pin, (unsigned)status.pir_output_pin,
             status.pir_active_high ? "high" : "low",
             (unsigned)status.pir_hold_ms,
             status.pir_active ? "yes" : "no",
             status.pir_output_owned ? "yes" : "no");
    } else {
        outf(out, context,
             "  PIR: unbound. Example: sensor pir 27 23 high 5000\n");
    }
    if (status.button_bound) {
        outf(out, context, "  Button: GPIO%u -> GPIO%u %s-active\n",
             (unsigned)status.button_pin, (unsigned)status.button_output_pin,
             status.button_active_low ? "low" : "high");
    } else {
        outf(out, context,
             "  Button: unbound. Example: sensor button 0 23 low\n");
    }

    automation_rule_info_t rules[AUTOMATION_MAX_RULES];
    size_t count = automation_rule_list(rules, AUTOMATION_MAX_RULES);
    outf(out, context, "  Threshold rules: %u\n", (unsigned)count);
    for (size_t i = 0; i < count; ++i) {
        automation_rule_info_t *rule = &rules[i];
        outf(out, context,
             "    %s: sensor=%s %s %ld (hyst %lu) -> GPIO%u, %s%s, "
             "value=%ld\n",
             rule->name, rule->sensor,
             rule->condition == AUTOMATION_ABOVE ? "above" : "below",
             (long)rule->threshold, (unsigned long)rule->hysteresis,
             (unsigned)rule->output_pin, rule->enabled ? "enabled" : "paused",
             rule->fault ? ", FAULT" : "", (long)rule->last_value);
    }
}

static void print_rule_list(serial_console_output_fn out, void *context)
{
    automation_rule_info_t rules[AUTOMATION_MAX_RULES];
    size_t count = automation_rule_list(rules, AUTOMATION_MAX_RULES);
    if (count == 0) {
        outf(out, context,
             "No threshold rules. Example: rule add fan temp above 3000 200 fan\n");
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        automation_rule_info_t *rule = &rules[i];
        outf(out, context,
             "%s: sensor=%s %s %ld (hyst %lu) -> GPIO%u, %s, active=%s, "
             "owns-output=%s%s, value=%ld\n",
             rule->name, rule->sensor,
             rule->condition == AUTOMATION_ABOVE ? "above" : "below",
             (long)rule->threshold, (unsigned long)rule->hysteresis,
             (unsigned)rule->output_pin, rule->enabled ? "enabled" : "paused",
             rule->active ? "yes" : "no",
             rule->output_owned ? "yes" : "no", rule->fault ? ", FAULT" : "",
             (long)rule->last_value);
    }
}

static void command_rule(serial_console_output_fn out, void *context, size_t argc,
                         char **argv)
{
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        print_rule_list(out, context);
        return;
    }
    if (strcmp(argv[1], "add") == 0) {
        if (argc != 8) {
            outf(out, context,
                 "Usage: rule add <name> <sensor> above|below <threshold> "
                 "<hysteresis> <output>\n");
            return;
        }
        automation_condition_t condition;
        if (strcmp(argv[4], "above") == 0) {
            condition = AUTOMATION_ABOVE;
        } else if (strcmp(argv[4], "below") == 0) {
            condition = AUTOMATION_BELOW;
        } else {
            outf(out, context, "Condition must be above or below.\n");
            return;
        }
        int32_t threshold = 0;
        uint32_t hysteresis = 0;
        if (!parse_i32(argv[5], &threshold) ||
            !parse_u32(argv[6], 0, 100000000, &hysteresis)) {
            outf(out, context, "Threshold/hysteresis must be integers.\n");
            return;
        }
        uint8_t output = 0;
        if (!resolve_target_pin(argv[7], &output)) {
            outf(out, context, "Invalid output GPIO or name '%s'.\n", argv[7]);
            return;
        }
        automation_rule_status_t status = automation_rule_add(
            argv[2], argv[3], output, condition, threshold, hysteresis);
        if (status == AUTOMATION_RULE_OK) {
            automation_set_enabled(true);
        }
        outf(out, context, "Rule '%s': %s.\n", argv[2],
             automation_rule_status_name(status));
        if (status == AUTOMATION_RULE_SENSOR_MISSING) {
            outf(out, context,
                 "The sensor must exist first: sensor add <name> <digital|adc1> "
                 "<gpio>.\n");
        } else if (status == AUTOMATION_RULE_OUTPUT_UNUSABLE) {
            outf(out, context,
                 "The output must be a configured, unlocked output pin.\n");
        }
        return;
    }

    if (strcmp(argv[1], "remove") == 0 || strcmp(argv[1], "enable") == 0 ||
        strcmp(argv[1], "disable") == 0 || strcmp(argv[1], "pause") == 0 ||
        strcmp(argv[1], "reset") == 0) {
        if (argc != 3) {
            outf(out, context,
                 "Usage: rule remove|enable|disable|reset <name>\n");
            return;
        }
        automation_rule_status_t status;
        if (strcmp(argv[1], "remove") == 0) {
            status = automation_rule_remove(argv[2]);
        } else if (strcmp(argv[1], "enable") == 0) {
            status = automation_rule_set_enabled(argv[2], true);
        } else if (strcmp(argv[1], "disable") == 0 ||
                   strcmp(argv[1], "pause") == 0) {
            status = automation_rule_set_enabled(argv[2], false);
        } else {
            status = automation_rule_reset(argv[2]);
        }
        outf(out, context, "Rule '%s' %s: %s.\n", argv[2], argv[1],
             automation_rule_status_name(status));
        if (status == AUTOMATION_RULE_OK &&
            strcmp(argv[1], "disable") != 0 && strcmp(argv[1], "pause") != 0) {
            outf(out, context, "Outputs are left unchanged by this operation.\n");
        }
        return;
    }

    outf(out, context,
         "Usage: rule add <name> <sensor> above|below <threshold> <hysteresis> "
         "<output>\n"
         "       rule remove|enable|disable|reset <name> | rules\n");
}

static void command_sensor(serial_console_output_fn out, void *context, size_t argc,
                           char **argv)
{
    if (argc < 2) {
        outf(out, context,
             "Usage: sensor add <name> <digital|adc1> <gpio> | sensor remove <name>\n"
             "       sensors | sensor read <name|all>\n"
             "       sensor pir <in> [out|led1] [high|low] [hold_ms]\n"
             "       sensor button <in> [out|led1] [high|low]\n"
             "       sensor status | sensor clear\n");
        return;
    }

    if (strcmp(argv[1], "pir") == 0) {
        if (argc < 3) {
            outf(out, context,
                 "Usage: sensor pir <input_gpio> [output|led1] [high|low] "
                 "[hold_ms]\n");
            return;
        }
        uint8_t input = 0;
        if (!resolve_target_pin(argv[2], &input)) {
            outf(out, context, "Invalid PIR input GPIO or name '%s'.\n", argv[2]);
            return;
        }
        uint8_t output = 0;
        bool have_output = false;
        bool active_high = true;
        uint32_t hold_ms = AUTOMATION_DEFAULT_PIR_HOLD_MS;
        for (size_t i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "high") == 0 ||
                strcmp(argv[i], "active-high") == 0) {
                active_high = true;
            } else if (strcmp(argv[i], "low") == 0 ||
                       strcmp(argv[i], "active-low") == 0) {
                active_high = false;
            } else if (strcmp(argv[i], "hold") == 0 && i + 1 < argc) {
                if (!parse_u32(argv[++i], 50, 3600000, &hold_ms)) {
                    outf(out, context, "Hold must be 50..3600000 ms.\n");
                    return;
                }
            } else if (!have_output && resolve_target_pin(argv[i], &output)) {
                have_output = true;
            } else if (parse_u32(argv[i], 50, 3600000, &hold_ms)) {
                /* bare numeric hold */
            } else {
                outf(out, context, "Unknown PIR option '%s'.\n", argv[i]);
                return;
            }
        }
        if (!have_output &&
            pin_manager_resolve_name("led1", &output) != PM_OK) {
            outf(out, context,
                 "No output specified and led1 is not configured. "
                 "Usage: sensor pir <in> <out> ...\n");
            return;
        }
        if (!automation_bind_pir(input, active_high, output, hold_ms)) {
            outf(out, context,
                 "PIR bind failed. Initialize an input pin and a configured "
                 "output first (input != output).\n");
            return;
        }
        automation_set_enabled(true);
        outf(out, context,
             "PIR bound: GPIO%u %s-active -> GPIO%u, hold %u ms. "
             "Automation enabled.\n",
             (unsigned)input, active_high ? "high" : "low", (unsigned)output,
             (unsigned)hold_ms);
        return;
    }

    if (strcmp(argv[1], "button") == 0) {
        if (argc < 3) {
            outf(out, context,
                 "Usage: sensor button <input_gpio> [output|led1] [high|low]\n");
            return;
        }
        uint8_t input = 0;
        if (!resolve_target_pin(argv[2], &input)) {
            outf(out, context, "Invalid button input GPIO or name '%s'.\n",
                 argv[2]);
            return;
        }
        uint8_t output = 0;
        bool have_output = false;
        bool active_high = false;
        for (size_t i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "high") == 0 ||
                strcmp(argv[i], "active-high") == 0) {
                active_high = true;
            } else if (strcmp(argv[i], "low") == 0 ||
                       strcmp(argv[i], "active-low") == 0) {
                active_high = false;
            } else if (!have_output && resolve_target_pin(argv[i], &output)) {
                have_output = true;
            } else {
                outf(out, context, "Unknown button option '%s'.\n", argv[i]);
                return;
            }
        }
        if (!have_output &&
            pin_manager_resolve_name("led1", &output) != PM_OK) {
            outf(out, context,
                 "No output specified and led1 is not configured. "
                 "Usage: sensor button <in> <out> ...\n");
            return;
        }
        if (!automation_bind_button(input, active_high, output)) {
            outf(out, context,
                 "Button bind failed. Initialize an input pin and a configured "
                 "output first (input != output).\n");
            return;
        }
        automation_set_enabled(true);
        outf(out, context,
             "Button bound: GPIO%u %s-active -> GPIO%u. Automation enabled.\n",
             (unsigned)input, active_high ? "high" : "low", (unsigned)output);
        return;
    }

    if (strcmp(argv[1], "status") == 0) {
        print_automation_status(out, context);
        return;
    }

    if (strcmp(argv[1], "clear") == 0) {
        automation_clear();
        outf(out, context,
             "Automation bindings cleared; any PIR-owned output was switched "
             "OFF through pin_manager.\n");
        return;
    }

    if (strcmp(argv[1], "add") == 0) {
        if (argc != 5) {
            outf(out, context, "Usage: sensor add <name> <digital|adc1> <gpio>\n");
            return;
        }
        uint32_t pin = 0;
        if (!parse_u32(argv[4], 0, PM_MAX_PINS - 1, &pin)) {
            outf(out, context, "GPIO must be 0..39.\n");
            return;
        }
        sensor_kind_t kind;
        if (strcmp(argv[3], "digital") == 0 || strcmp(argv[3], "gpio") == 0) {
            kind = SENSOR_KIND_DIGITAL;
        } else if (strcmp(argv[3], "adc1") == 0 || strcmp(argv[3], "adc") == 0) {
            kind = SENSOR_KIND_ADC1;
        } else {
            outf(out, context, "Sensor kind must be digital or adc1.\n");
            return;
        }
        sensor_status_t status = sensor_manager_add(argv[2], kind, (uint8_t)pin);
        outf(out, context, "Sensor '%s': %s.\n", argv[2],
             sensor_status_name(status));
        if (status == SENSOR_ERR_PIN_NOT_READY) {
            outf(out, context, "Initialize the pin first, e.g. pin %u input\n",
                 (unsigned)pin);
        }
        return;
    }
    if (strcmp(argv[1], "remove") == 0) {
        if (argc != 3) {
            outf(out, context, "Usage: sensor remove <name>\n");
            return;
        }
        outf(out, context, "Sensor '%s' remove: %s.\n", argv[2],
             sensor_status_name(sensor_manager_remove(argv[2])));
        return;
    }
    if (strcmp(argv[1], "read") == 0) {
        if (argc != 3) {
            outf(out, context, "Usage: sensor read <name|all>\n");
            return;
        }
        if (strcmp(argv[2], "all") == 0) {
            size_t valid = sensor_manager_sample_all();
            sensor_sample_t sensors[SENSOR_MAX];
            size_t count = sensor_manager_list(sensors, SENSOR_MAX);
            for (size_t i = 0; i < count; ++i) {
                print_sensor_line(&sensors[i], out, context);
            }
            outf(out, context, "%u/%u sensor(s) valid.\n", (unsigned)valid,
                 (unsigned)count);
            return;
        }
        sensor_sample_t sensor;
        sensor_status_t status = sensor_manager_sample(argv[2], &sensor);
        if (status != SENSOR_OK) {
            outf(out, context, "Sensor '%s': %s.\n", argv[2],
                 sensor_status_name(status));
            return;
        }
        print_sensor_line(&sensor, out, context);
        return;
    }
    outf(out, context, "Unknown sensor action '%s'.\n", argv[1]);
}

static void command_automation(serial_console_output_fn out, void *context,
                               size_t argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        print_automation_status(out, context);
        return;
    }
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "resume") == 0 ||
        strcmp(argv[1], "enable") == 0) {
        automation_set_enabled(true);
        outf(out, context, "Automation enabled.\n");
        return;
    }
    if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "pause") == 0 ||
        strcmp(argv[1], "disable") == 0) {
        automation_set_enabled(false);
        outf(out, context,
             "Automation paused; outputs were left unchanged.\n");
        return;
    }
    if (strcmp(argv[1], "clear") == 0) {
        automation_clear();
        outf(out, context,
             "Automation bindings and rules cleared; outputs were left "
             "unchanged.\n");
        return;
    }
    outf(out, context,
         "Usage: automation on|off|status|clear (pause/resume aliases)\n");
}

static void command_log(serial_console_output_fn out, void *context, size_t argc,
                        char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
        pin_manager_clear_log();
        outf(out, context, "Audit log cleared.\n");
        return;
    }
    uint32_t wanted = 10;
    if (argc >= 2 && !parse_u32(argv[1], 1, PM_LOG_CAPACITY, &wanted)) {
        outf(out, context, "Usage: log [1..%u] | log clear\n",
             (unsigned)PM_LOG_CAPACITY);
        return;
    }
    pm_action_log_entry_t entries[PM_LOG_CAPACITY];
    size_t count = pin_manager_get_log(entries, PM_LOG_CAPACITY);
    if (count == 0) {
        outf(out, context, "Audit log is empty.\n");
        return;
    }
    size_t start = count > wanted ? count - wanted : 0;
    for (size_t i = start; i < count; ++i) {
        pm_action_log_entry_t *e = &entries[i];
        outf(out, context,
             "  #%lu %8lums %-10s %-20s %-16s %-16s -> %s (%.2f)\n",
             (unsigned long)e->seq, (unsigned long)e->timestamp_ms,
             pin_origin_name(e->origin), e->intent, e->target, e->source,
             e->result, (double)e->confidence);
    }
}

static void command_classify(serial_console_output_fn out, void *context,
                             const char *text)
{
    command_result_t result;
    command_status_t status = command_engine_classify(text, &result);
    outf(out, context,
         "classify: status=%s intent=%s confidence=%.3f margin=%.3f "
         "threshold=%.2f rejected=%s guard=%s\n",
         command_status_name(status), command_intent_name(result.intent),
         (double)result.confidence, (double)result.margin,
         (double)result.threshold, result.rejected ? "yes" : "no",
         result.guard_rejected ? "yes" : "no");
    if (result.message[0] != '\0') {
        outf(out, context, "%s\n", result.message);
    }
}

static void command_do(serial_console_output_fn out, void *context,
                       const char *text)
{
    command_result_t result;
    command_status_t status =
        command_engine_handle_text(text, PM_ORIGIN_AI, &result);
    if (result.intent != CMD_INTENT_NONE && status != CMD_AI_DISABLED) {
        outf(out, context, "intent=%s confidence=%.3f margin=%.3f\n",
             command_intent_name(result.intent), (double)result.confidence,
             (double)result.margin);
    }
    if (result.message[0] != '\0') {
        outf(out, context, "%s\n", result.message);
    }
    if (status == CMD_CONFIRMATION_REQUIRED) {
        outf(out, context,
             "Use 'confirm' from this local console or 'cancel'. No network "
             "confirmation is accepted.\n");
    }
}

static void command_bench(serial_console_output_fn out, void *context, size_t argc,
                          char **argv)
{
    uint32_t iterations = 1000;
    if (argc >= 2 && !parse_u32(argv[1], 1, 10000, &iterations)) {
        outf(out, context, "Usage: bench [1..10000]\n");
        return;
    }
    uint32_t elapsed_us = 0;
    uint32_t predictions = 0;
    if (!command_engine_benchmark(iterations, &elapsed_us, &predictions)) {
        outf(out, context, "Benchmark unavailable: TinyML is not compiled in.\n");
        return;
    }
    if (predictions == 0 || elapsed_us == 0) {
        outf(out, context, "Benchmark failed to complete.\n");
        return;
    }
    outf(out, context,
         "Measured on this runtime: %lu predictions in %lu us (%.2f us/pred, "
         "%.0f pred/s)\n",
         (unsigned long)predictions, (unsigned long)elapsed_us,
         (double)elapsed_us / (double)predictions,
         1000000.0 * (double)predictions / (double)elapsed_us);
}

static void command_confirm(serial_console_output_fn out, void *context)
{
    if (command_engine_confirmation_pending()) {
        command_result_t result;
        command_status_t status = command_engine_confirm(&result);
        outf(out, context, "confirm: %s\n", command_status_name(status));
        if (result.message[0] != '\0') {
            outf(out, context, "%s\n", result.message);
        }
        return;
    }
    int kind = (int)s_pending.kind;
    if (kind == CONSOLE_PENDING_NONE) {
        outf(out, context, "Nothing is waiting for confirmation.\n");
        return;
    }
    char group[PM_GROUP_NAME_MAX];
    snprintf(group, sizeof(group), "%s", s_pending.group);
    console_pending_clear();
    if (kind == CONSOLE_PENDING_ALL_ON) {
        size_t affected = 0;
        pm_status_t status =
            pin_manager_outputs_all_on(true, "console-confirm", &affected);
        outf(out, context, "All approved outputs ON: %s, %u output(s).\n",
             pin_manager_status_name(status), (unsigned)affected);
        return;
    }
    if (kind == CONSOLE_PENDING_GROUP_ON) {
        size_t affected = 0;
        pm_status_t status = pin_manager_group_apply(
            group, true, true, PM_ORIGIN_LOCAL, "GROUP_ON", 1.0f,
            "console-confirm", &affected);
        outf(out, context, "Group '%s' ON: %s, %u output(s).\n", group,
             pin_manager_status_name(status), (unsigned)affected);
    }
}

static void command_config(serial_console_output_fn out, void *context, size_t argc,
                           char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        config_store_status_t status = config_store_probe();
        outf(out, context, "Config: %s (%s).\n",
             config_store_status_name(status), config_store_last_detail());
        return;
    }
    if (strcmp(argv[1], "erase") == 0) {
        config_store_status_t status = config_store_erase();
        outf(out, context, "Config erase: %s (%s).\n",
             config_store_status_name(status), config_store_last_detail());
        return;
    }
    outf(out, context, "Usage: config status | config erase\n");
}

int serial_console_execute_line(const char *line, serial_console_output_fn out,
                                void *context)
{
    if (line == NULL) {
        return -1;
    }
    char raw[CONSOLE_LINE_MAX];
    size_t length = strlen(line);
    if (length >= sizeof(raw)) {
        outf(out, context, "Command line too long (max %u bytes).\n",
             (unsigned)(sizeof(raw) - 1));
        return -1;
    }
    memcpy(raw, line, length + 1);
    char *trimmed = skip_ws(raw);
    size_t trimmed_len = strlen(trimmed);
    while (trimmed_len > 0 && isspace((unsigned char)trimmed[trimmed_len - 1])) {
        trimmed[--trimmed_len] = '\0';
    }
    if (trimmed_len == 0) {
        return -1;
    }

    char work[CONSOLE_LINE_MAX];
    memcpy(work, trimmed, trimmed_len + 1);
    char *argv[CONSOLE_MAX_ARGS];
    size_t argc = tokenize(work, argv, CONSOLE_MAX_ARGS);
    if (argc == (size_t)-1 || argc == 0) {
        outf(out, context, "Could not parse command (check quotes).\n");
        return -1;
    }

    console_pending_expire_if_needed(out, context);
    bool pending = command_engine_confirmation_pending() || console_pending_active();
    if (pending && strcmp(argv[0], "confirm") != 0 &&
        strcmp(argv[0], "cancel") != 0 && strcmp(argv[0], "help") != 0) {
        outf(out, context,
             "A confirmation is pending. Type 'confirm' or 'cancel' first.\n");
        return 0;
    }

    if (strcmp(argv[0], "help") == 0) {
        print_help(out, context);
        return 0;
    }
    if (strcmp(argv[0], "pins") == 0) {
        command_pins(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "devices") == 0 || strcmp(argv[0], "list") == 0) {
        command_devices(out, context);
        return 0;
    }
    if (strcmp(argv[0], "status") == 0) {
        command_status(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "gpio") == 0) {
        command_gpio(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "pin") == 0) {
        command_pin(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "deinit") == 0) {
        if (argc != 2) {
            outf(out, context, "Usage: deinit <gpio>\n");
            return 0;
        }
        uint32_t pin = 0;
        if (!parse_u32(argv[1], 0, PM_MAX_PINS - 1, &pin)) {
            outf(out, context, "GPIO must be 0..39.\n");
            return 0;
        }
        outf(out, context, "GPIO%u deinit: %s.\n", (unsigned)pin,
             pin_manager_status_name(pin_manager_pin_deinit((uint8_t)pin)));
        return 0;
    }
    if (strcmp(argv[0], "assign") == 0) {
        command_assign(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "alias") == 0) {
        if (argc != 3) {
            outf(out, context, "Usage: alias <gpio> <alias>\n");
            return 0;
        }
        uint32_t pin = 0;
        if (!parse_u32(argv[1], 0, PM_MAX_PINS - 1, &pin)) {
            outf(out, context, "GPIO must be 0..39.\n");
            return 0;
        }
        outf(out, context, "Alias '%s' for GPIO%u: %s.\n", argv[2],
             (unsigned)pin,
             pin_manager_status_name(pin_manager_add_alias((uint8_t)pin, argv[2])));
        return 0;
    }
    if (strcmp(argv[0], "release") == 0) {
        if (argc != 2) {
            outf(out, context, "Usage: release <gpio>\n");
            return 0;
        }
        uint32_t pin = 0;
        if (!parse_u32(argv[1], 0, PM_MAX_PINS - 1, &pin)) {
            outf(out, context, "GPIO must be 0..39.\n");
            return 0;
        }
        outf(out, context, "GPIO%u release: %s.\n", (unsigned)pin,
             pin_manager_status_name(pin_manager_release((uint8_t)pin)));
        return 0;
    }
    if (strcmp(argv[0], "lock") == 0) {
        if (argc != 3) {
            outf(out, context, "Usage: lock <gpio> on|off\n");
            return 0;
        }
        uint32_t pin = 0;
        bool locked = false;
        if (!parse_u32(argv[1], 0, PM_MAX_PINS - 1, &pin) ||
            !parse_on_off(argv[2], &locked)) {
            outf(out, context, "Expected: lock <gpio> on|off\n");
            return 0;
        }
        outf(out, context, "GPIO%u lock=%s: %s.\n", (unsigned)pin,
             locked ? "on" : "off",
             pin_manager_status_name(
                 pin_manager_set_locked((uint8_t)pin, locked)));
        return 0;
    }
    if (strcmp(argv[0], "test") == 0) {
        if (argc != 3) {
            outf(out, context, "Usage: test <gpio> on|off\n");
            return 0;
        }
        uint32_t pin = 0;
        bool approved = false;
        if (!parse_u32(argv[1], 0, PM_MAX_PINS - 1, &pin) ||
            !parse_on_off(argv[2], &approved)) {
            outf(out, context, "Expected: test <gpio> on|off\n");
            return 0;
        }
        outf(out, context, "GPIO%u bulk-test=%s: %s.\n", (unsigned)pin,
             approved ? "approved" : "revoked",
             pin_manager_status_name(
                 pin_manager_set_test_approved((uint8_t)pin, approved)));
        return 0;
    }
    if (strcmp(argv[0], "ai") == 0) {
        bool known = argc >= 2 &&
                     (strcmp(argv[1], "status") == 0 ||
                      strcmp(argv[1], "on") == 0 ||
                      strcmp(argv[1], "off") == 0 ||
                      strcmp(argv[1], "allow") == 0 ||
                      strcmp(argv[1], "model") == 0);
        if (argc >= 2 && !known) {
            char *text = trimmed;
            while (*text != '\0' && !isspace((unsigned char)*text)) {
                ++text;
            }
            text = skip_ws(text);
            if (*text != '\0') {
                command_do(out, context, text);
                return 0;
            }
        }
        command_ai(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "group") == 0) {
        command_group(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "all") == 0) {
        if (argc == 2 && strcmp(argv[1], "on") == 0) {
            command_all(out, context, true);
        } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
            command_all(out, context, false);
        } else {
            outf(out, context, "Usage: all on|off\n");
        }
        return 0;
    }
    if (strcmp(argv[0], "outputs") == 0) {
        if (argc == 3 && strcmp(argv[1], "all") == 0) {
            if (strcmp(argv[2], "on") == 0) {
                command_all(out, context, true);
            } else if (strcmp(argv[2], "off") == 0) {
                command_all(out, context, false);
            } else {
                outf(out, context, "Usage: outputs all on|off\n");
            }
        } else {
            outf(out, context, "Usage: outputs all on|off\n");
        }
        return 0;
    }
    if (strcmp(argv[0], "sensor") == 0) {
        command_sensor(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "sensors") == 0) {
        sensor_sample_t sensors[SENSOR_MAX];
        size_t count = sensor_manager_list(sensors, SENSOR_MAX);
        if (count == 0) {
            outf(out, context,
                 "No sensors. Example: pin 34 input; sensor add light adc1 34\n");
            return 0;
        }
        for (size_t i = 0; i < count; ++i) {
            print_sensor_line(&sensors[i], out, context);
        }
        return 0;
    }
    if (strcmp(argv[0], "automation") == 0) {
        command_automation(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "rule") == 0) {
        command_rule(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "rules") == 0) {
        print_rule_list(out, context);
        return 0;
    }
    if (strcmp(argv[0], "log") == 0) {
        command_log(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "save") == 0) {
        config_store_status_t status = config_store_save();
        outf(out, context, "Config save: %s (%s).\n",
             config_store_status_name(status), config_store_last_detail());
        return 0;
    }
    if (strcmp(argv[0], "load") == 0) {
        command_engine_cancel();
        console_pending_clear();
        config_store_status_t status = config_store_load();
        outf(out, context,
             "Config load: %s (%s). Outputs are initialized OFF.\n",
             config_store_status_name(status), config_store_last_detail());
        return 0;
    }
    if (strcmp(argv[0], "config") == 0) {
        command_config(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "classify") == 0) {
        char *text = trimmed;
        while (*text != '\0' && !isspace((unsigned char)*text)) {
            ++text;
        }
        text = skip_ws(text);
        if (*text == '\0') {
            outf(out, context, "Usage: classify <text>\n");
            return 0;
        }
        command_classify(out, context, text);
        return 0;
    }
    if (strcmp(argv[0], "do") == 0) {
        char *text = trimmed;
        while (*text != '\0' && !isspace((unsigned char)*text)) {
            ++text;
        }
        text = skip_ws(text);
        if (*text == '\0') {
            outf(out, context, "Usage: do <natural language>\n");
            return 0;
        }
        command_do(out, context, text);
        return 0;
    }
    if (strcmp(argv[0], "confirm") == 0) {
        command_confirm(out, context);
        return 0;
    }
    if (strcmp(argv[0], "cancel") == 0) {
        uint8_t active_pin = 0;
        if (pin_manager_blink_is_active(&active_pin)) {
            (void)pin_manager_blink_cancel(out, context);
        }
        command_engine_cancel();
        console_pending_clear();
        outf(out, context, "Pending confirmation cancelled.\n");
        return 0;
    }
    if (strcmp(argv[0], "bench") == 0) {
        command_bench(out, context, argc, argv);
        return 0;
    }
    if (strcmp(argv[0], "model") == 0) {
        char info[384];
        command_engine_model_info(info, sizeof(info));
        outf(out, context, "%s\n", info);
        return 0;
    }
    if (strcmp(argv[0], "board") == 0) {
        command_board(out, context);
        return 0;
    }
    if (strcmp(argv[0], "heap") == 0 || strcmp(argv[0], "mem") == 0) {
        board_runtime_info_t info;
        board_profile_get_runtime_info(&info);
        outf(out, context, "Heap: free=%u min=%u largest=%u\n",
             (unsigned)info.free_heap_bytes,
             (unsigned)info.minimum_free_heap_bytes,
             (unsigned)info.largest_free_block_bytes);
#ifdef ESP_PLATFORM
        if (s_console_task_handle != NULL) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(s_console_task_handle);
            outf(out, context, "Console task stack min free: %u bytes\n",
                 (unsigned)(hwm * sizeof(StackType_t)));
        }
#endif
        return 0;
    }
    if (strcmp(argv[0], "reboot") == 0) {
#ifdef ESP_PLATFORM
        outf(out, context, "Rebooting...\n");
        esp_restart();
#else
        outf(out, context, "reboot is unavailable in the host build.\n");
#endif
        return 0;
    }

    outf(out, context, "Unknown command '%s'. Type 'help'.\n", argv[0]);
    return -1;
}

#ifdef ESP_PLATFORM

static void console_output(void *context, const char *text)
{
    (void)context;
    fputs(text, stdout);
    fflush(stdout);
}

static void console_task(void *argument)
{
    (void)argument;
    outf(console_output, NULL,
         "\nTinyAI ESP32 controller. Type 'help' for commands.\n");
    char line[CONSOLE_LINE_MAX];
    size_t length = 0;
    bool last_was_cr = false;
    for (;;) {
        fputs("tinyai> ", stdout);
        fflush(stdout);
        length = 0;
        last_was_cr = false;
        for (;;) {
            uint8_t ch = 0;
            int read = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(200));
            if (read <= 0) {
                continue;
            }
            if (ch == '\n' && last_was_cr) {
                last_was_cr = false;
                continue;
            }
            if (ch == '\r' || ch == '\n') {
                last_was_cr = ch == '\r';
                fputs("\r\n", stdout);
                break;
            }
            if (ch == 8 || ch == 127) {
                if (length > 0) {
                    length--;
                    fputs("\b \b", stdout);
                    fflush(stdout);
                }
                continue;
            }
            if (ch >= 32 && ch < 127) {
                if (length + 1 < sizeof(line)) {
                    line[length++] = (char)ch;
                    fputc((int)ch, stdout);
                    fflush(stdout);
                } else {
                    fputc('\a', stdout);
                    fflush(stdout);
                }
            }
        }
        line[length] = '\0';
        serial_console_execute_line(line, console_output, NULL);
    }
}

int serial_console_start(void)
{
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_config_t config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        esp_err_t err = uart_param_config(UART_NUM_0, &config);
        if (err == ESP_OK) {
            err = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
        }
        if (err != ESP_OK) {
            return -1;
        }
    }
    uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
    uart_vfs_dev_use_driver(UART_NUM_0);

    BaseType_t created = xTaskCreate(console_task, "tinyai_cli", CONSOLE_TASK_STACK,
                                     NULL, 2, &s_console_task_handle);
    return created == pdPASS ? 0 : -1;
}

uint32_t serial_console_get_stack_high_water_mark(void)
{
    if (s_console_task_handle != NULL) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(s_console_task_handle);
        return (uint32_t)(hwm * sizeof(StackType_t));
    }
    return 0;
}

#else

int serial_console_start(void)
{
    return -1;
}

uint32_t serial_console_get_stack_high_water_mark(void)
{
    return 0;
}

#endif
