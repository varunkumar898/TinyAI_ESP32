#include "board_profile.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
static const char *TAG = "board";
#endif

typedef struct {
    bool present;
    bool input_only;
    bool adc1;
    bool adc2;
    bool boot_strap;
    bool flash;
    bool uart0;
    bool module_reserved;
} pin_static_caps_t;

static const pin_static_caps_t s_caps[BOARD_PROFILE_PIN_COUNT] = {
    [0]  = {true, false, false, true,  true,  false, false, false},
    [1]  = {true, false, false, false, false, false, true,  false},
    [2]  = {true, false, false, true,  true,  false, false, false},
    [3]  = {true, false, false, false, false, false, true,  false},
    [4]  = {true, false, false, true,  false, false, false, false},
    [5]  = {true, false, false, false, true,  false, false, false},
    [6]  = {true, false, false, false, false, true,  false, false},
    [7]  = {true, false, false, false, false, true,  false, false},
    [8]  = {true, false, false, false, false, true,  false, false},
    [9]  = {true, false, false, false, false, true,  false, false},
    [10] = {true, false, false, false, false, true,  false, false},
    [11] = {true, false, false, false, false, true,  false, false},
    [12] = {true, false, false, true,  true,  false, false, false},
    [13] = {true, false, false, true,  false, false, false, false},
    [14] = {true, false, false, true,  false, false, false, false},
    [15] = {true, false, false, true,  true,  false, false, false},
    [16] = {true, false, false, false, false, false, false, true},
    [17] = {true, false, false, false, false, false, false, true},
    [18] = {true, false, false, false, false, false, false, false},
    [19] = {true, false, false, false, false, false, false, false},
    [20] = {false, false, false, false, false, false, false, false},
    [21] = {true, false, false, false, false, false, false, false},
    [22] = {true, false, false, false, false, false, false, false},
    [23] = {true, false, false, false, false, false, false, false},
    [24] = {false, false, false, false, false, false, false, false},
    [25] = {true, false, false, true,  false, false, false, false},
    [26] = {true, false, false, true,  false, false, false, false},
    [27] = {true, false, false, true,  false, false, false, false},
    [28] = {false, false, false, false, false, false, false, false},
    [29] = {false, false, false, false, false, false, false, false},
    [30] = {false, false, false, false, false, false, false, false},
    [31] = {false, false, false, false, false, false, false, false},
    [32] = {true, false, true,  false, false, false, false, false},
    [33] = {true, false, true,  false, false, false, false, false},
    [34] = {true, true,  true,  false, false, false, false, false},
    [35] = {true, true,  true,  false, false, false, false, false},
    [36] = {true, true,  true,  false, false, false, false, false},
    [37] = {true, true,  true,  false, false, false, false, false},
    [38] = {true, true,  true,  false, false, false, false, false},
    [39] = {true, true,  true,  false, false, false, false, false},
};

static const char *pin_reserved_reason(uint8_t pin)
{
    if (pin >= BOARD_PROFILE_PIN_COUNT) {
        return "invalid GPIO";
    }
    if (s_caps[pin].flash) {
        return "SPI flash";
    }
    if (s_caps[pin].uart0) {
        return "UART0 console/programming";
    }
    if (s_caps[pin].module_reserved) {
        return "GPIO16/17 module-variant specific (blocked until verified)";
    }
    if (s_caps[pin].boot_strap) {
        return "boot strapping pin";
    }
    if (!s_caps[pin].present) {
        return "not bonded out on WROOM-32";
    }
    return "";
}

void board_profile_init(void)
{
#ifdef ESP_PLATFORM
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "board: %s", BOARD_PROFILE_NAME);
    ESP_LOGI(TAG, "module: ESP32-WROOM-32 (assume no PSRAM; detected PSRAM=%u bytes)",
             (unsigned)psram);
    ESP_LOGI(TAG, "chip: ESP32 rev %u.%u, %u core(s), flash %u bytes",
             (unsigned)((chip.revision >> 8) & 0xff), (unsigned)(chip.revision & 0xff),
             (unsigned)chip.cores, (unsigned)flash_size);
    ESP_LOGI(TAG, "heap at boot: free=%u min=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (psram == 0) {
        ESP_LOGW(TAG, "PSRAM: none detected; buffers are static or internal SRAM only");
    }
    ESP_LOGW(TAG, "clone boards differ: verify your board's pin header before wiring");
#endif
}

void board_profile_get_runtime_info(board_runtime_info_t *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    info->board_name = BOARD_PROFILE_NAME;
    info->module_name = "ESP32-WROOM-32";
    info->flash_bytes = BOARD_PROFILE_FLASH_ASSUMED_BYTES;
    info->psram_present = BOARD_PROFILE_PSRAM_ASSUMED;
#ifdef ESP_PLATFORM
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        info->flash_bytes = flash_size;
    }
    info->psram_present = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    info->free_heap_bytes = esp_get_free_heap_size();
    info->minimum_free_heap_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    info->largest_free_block_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    info->chip_cores = chip.cores;
    info->chip_revision = chip.revision;
#endif
}

bool board_profile_get_pin_caps(uint8_t pin, board_pin_caps_t *caps)
{
    if (caps == NULL || pin >= BOARD_PROFILE_PIN_COUNT) {
        return false;
    }
    const pin_static_caps_t *s = &s_caps[pin];
    memset(caps, 0, sizeof(*caps));
    caps->present = s->present;
    caps->input_capable = s->present;
    caps->output_capable = s->present && !s->input_only;
    caps->pull_up = s->present && !s->input_only;
    caps->pull_down = s->present && !s->input_only;
    caps->adc1 = s->adc1;
    caps->adc2 = s->adc2;
    caps->boot_strap = s->boot_strap;
    caps->module_reserved = s->module_reserved;
    caps->reserved = s->flash || s->uart0 || s->module_reserved || s->boot_strap;
    caps->reserved_reason = pin_reserved_reason(pin);
    return s->present;
}

board_pin_issue_t board_profile_check_pin(uint8_t pin, bool output, bool pull_up,
                                          bool pull_down)
{
    if (pin >= BOARD_PROFILE_PIN_COUNT) {
        return BOARD_PIN_INVALID;
    }
    const pin_static_caps_t *s = &s_caps[pin];
    if (!s->present) {
        return BOARD_PIN_NOT_PRESENT;
    }
    if (s->flash) {
        return BOARD_PIN_FLASH;
    }
    if (s->uart0) {
        return BOARD_PIN_UART0;
    }
    if (s->module_reserved) {
        return BOARD_PIN_MODULE_RESERVED;
    }
    if (s->boot_strap) {
        return BOARD_PIN_STRAPPING;
    }
    if (output && s->input_only) {
        return BOARD_PIN_INPUT_ONLY;
    }
    if ((pull_up || pull_down) && s->input_only) {
        return BOARD_PIN_NO_PULL;
    }
    return BOARD_PIN_OK;
}

const char *board_profile_issue_name(board_pin_issue_t issue)
{
    switch (issue) {
    case BOARD_PIN_OK: return "ok";
    case BOARD_PIN_NOT_PRESENT: return "not-present";
    case BOARD_PIN_FLASH: return "flash-reserved";
    case BOARD_PIN_STRAPPING: return "boot-strapping";
    case BOARD_PIN_UART0: return "uart0-reserved";
    case BOARD_PIN_MODULE_RESERVED: return "module-variant-reserved";
    case BOARD_PIN_INPUT_ONLY: return "input-only";
    case BOARD_PIN_NO_PULL: return "no-internal-pull";
    case BOARD_PIN_INVALID: return "invalid-pin";
    default: return "unknown";
    }
}

const char *board_profile_issue_hint(board_pin_issue_t issue)
{
    switch (issue) {
    case BOARD_PIN_FLASH:
        return "GPIO6-11 are connected to the SPI flash.";
    case BOARD_PIN_STRAPPING:
        return "GPIO0/2/5/12/15 select boot mode; they are blocked by policy.";
    case BOARD_PIN_UART0:
        return "GPIO1/3 are the UART0 console and programming pins.";
    case BOARD_PIN_MODULE_RESERVED:
        return "GPIO16/17 are reserved until the module variant is verified.";
    case BOARD_PIN_INPUT_ONLY:
        return "GPIO34-39 have input-only pads.";
    case BOARD_PIN_NO_PULL:
        return "GPIO34-39 do not have software-controlled pull resistors.";
    case BOARD_PIN_NOT_PRESENT:
        return "This GPIO is not available on ESP32-WROOM-32.";
    default:
        return "";
    }
}

void board_profile_format_pin(uint8_t pin, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    board_pin_caps_t caps;
    if (pin >= BOARD_PROFILE_PIN_COUNT || !board_profile_get_pin_caps(pin, &caps)) {
        snprintf(out, out_size, "GPIO%u: not present", (unsigned)pin);
        return;
    }
    char modes[48] = "";
    if (caps.input_capable) {
        strncat(modes, "in", sizeof(modes) - strlen(modes) - 1);
    }
    if (caps.output_capable) {
        strncat(modes, modes[0] ? "/out" : "out", sizeof(modes) - strlen(modes) - 1);
    }
    if (caps.pull_up) {
        strncat(modes, "/pull", sizeof(modes) - strlen(modes) - 1);
    }
    snprintf(out, out_size, "GPIO%u: %s%s%s%s%s", (unsigned)pin, modes,
             caps.adc1 ? " adc1" : "", caps.adc2 ? " adc2" : "",
             caps.boot_strap ? " strap" : "",
             caps.reserved ? " RESERVED" : "");
}
