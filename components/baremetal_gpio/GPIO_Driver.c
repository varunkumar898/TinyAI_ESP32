#include "GPIO_Driver.h"
#include "GPIO_Registers.h"

#include <string.h>

#include "board_profile.h"

#define PIN_COUNT 40u
#define MODE_UNINITIALIZED 0xFFu

typedef struct {
    GPIO_Mode mode;
    GPIO_Pull pull;
} pin_state_t;

static pin_state_t s_state[PIN_COUNT];
static bool s_initialized = false;

static uint64_t bit64(uint8_t pin)
{
    return (uint64_t)1u << pin;
}

static void ensure_initialized(void)
{
    if (!s_initialized) {
        for (uint8_t pin = 0; pin < PIN_COUNT; ++pin) {
            s_state[pin].mode = (GPIO_Mode)MODE_UNINITIALIZED;
            s_state[pin].pull = GPIO_PULL_NONE;
        }
        s_initialized = true;
    }
}

static bool valid_mode(GPIO_Mode mode)
{
    return mode == GPIO_MODE_INPUT || mode == GPIO_MODE_OUTPUT ||
           mode == GPIO_MODE_INPUT_PULLUP || mode == GPIO_MODE_INPUT_PULLDOWN;
}

static bool valid_pull(GPIO_Pull pull)
{
    return pull == GPIO_PULL_NONE || pull == GPIO_PULL_UP ||
           pull == GPIO_PULL_DOWN;
}

static GPIO_Pull pull_for_mode(GPIO_Mode mode, GPIO_Pull requested)
{
    if (requested != GPIO_PULL_NONE) {
        return requested;
    }
    if (mode == GPIO_MODE_INPUT_PULLUP) {
        return GPIO_PULL_UP;
    }
    if (mode == GPIO_MODE_INPUT_PULLDOWN) {
        return GPIO_PULL_DOWN;
    }
    return GPIO_PULL_NONE;
}

GPIO_Status GPIO_GetCapabilities(uint8_t pin, GPIO_Capabilities *caps)
{
    if (caps == NULL) {
        return GPIO_ERR_INVALID_ARGUMENT;
    }
    memset(caps, 0, sizeof(*caps));
    if (pin >= PIN_COUNT) {
        return GPIO_ERR_INVALID_PIN;
    }

    board_pin_caps_t board;
    if (!board_profile_get_pin_caps(pin, &board)) {
        /* The pin index is valid on the SoC but not bonded out on this module. */
        caps->reserved = true;
        return GPIO_OK;
    }

    caps->exists = board.present;
    caps->input_capable = board.input_capable;
    caps->output_capable = board.output_capable;
    caps->pullup_capable = board.pull_up;
    caps->pulldown_capable = board.pull_down;
    caps->adc1_capable = board.adc1;
    caps->adc2_capable = board.adc2;
    caps->rtc_capable = (pin <= 15u) || (pin >= 25u && pin <= 27u) ||
                        (pin >= 32u && pin <= 39u);
    caps->reserved = board.reserved;
    return GPIO_OK;
}

bool GPIO_IsReserved(uint8_t pin)
{
    if (pin >= PIN_COUNT) {
        return true;
    }
    GPIO_Capabilities caps;
    if (GPIO_GetCapabilities(pin, &caps) != GPIO_OK) {
        return true;
    }
    return !caps.exists || caps.reserved;
}

GPIO_Status GPIO_ValidatePin(uint8_t pin, GPIO_Mode mode, GPIO_Pull pull,
                             const char **reason)
{
    if (reason != NULL) {
        *reason = "";
    }
    if (pin >= PIN_COUNT) {
        if (reason != NULL) {
            *reason = "GPIO must be 0..39";
        }
        return GPIO_ERR_INVALID_PIN;
    }
    if (!valid_mode(mode)) {
        if (reason != NULL) {
            *reason = "unsupported GPIO mode";
        }
        return GPIO_ERR_UNSUPPORTED_MODE;
    }
    if (!valid_pull(pull)) {
        if (reason != NULL) {
            *reason = "unsupported pull mode";
        }
        return GPIO_ERR_UNSUPPORTED_PULL;
    }

    GPIO_Capabilities caps;
    GPIO_Status caps_status = GPIO_GetCapabilities(pin, &caps);
    if (caps_status != GPIO_OK) {
        return caps_status;
    }
    if (!caps.exists) {
        if (reason != NULL) {
            *reason = "GPIO is not bonded out on this module";
        }
        return GPIO_ERR_RESERVED;
    }

    GPIO_Pull effective_pull = pull_for_mode(mode, pull);
    bool output = mode == GPIO_MODE_OUTPUT;
    board_pin_issue_t issue = board_profile_check_pin(
        pin, output, effective_pull == GPIO_PULL_UP,
        effective_pull == GPIO_PULL_DOWN);
    if (issue != BOARD_PIN_OK) {
        if (reason != NULL) {
            const char *hint = board_profile_issue_hint(issue);
            *reason = (hint != NULL && hint[0] != '\0')
                          ? hint
                          : board_profile_issue_name(issue);
        }
        switch (issue) {
        case BOARD_PIN_INPUT_ONLY:
            return GPIO_ERR_UNSUPPORTED_MODE;
        case BOARD_PIN_NO_PULL:
            return GPIO_ERR_UNSUPPORTED_PULL;
        case BOARD_PIN_INVALID:
            return GPIO_ERR_INVALID_PIN;
        default:
            return GPIO_ERR_RESERVED;
        }
    }

    if (output && effective_pull != GPIO_PULL_NONE) {
        if (reason != NULL) {
            *reason = "output mode does not accept an internal pull";
        }
        return GPIO_ERR_UNSUPPORTED_PULL;
    }
    if (mode == GPIO_MODE_INPUT_PULLUP && pull == GPIO_PULL_DOWN) {
        if (reason != NULL) {
            *reason = "input-pullup conflicts with a pull-down request";
        }
        return GPIO_ERR_UNSUPPORTED_PULL;
    }
    if (mode == GPIO_MODE_INPUT_PULLDOWN && pull == GPIO_PULL_UP) {
        if (reason != NULL) {
            *reason = "input-pulldown conflicts with a pull-up request";
        }
        return GPIO_ERR_UNSUPPORTED_PULL;
    }
    return GPIO_OK;
}

GPIO_Status GPIO_InitFull(uint8_t pin, GPIO_Mode mode, GPIO_Pull pull,
                          GPIO_Level initial_level)
{
    ensure_initialized();
    if (initial_level != GPIO_LEVEL_LOW && initial_level != GPIO_LEVEL_HIGH) {
        return GPIO_ERR_INVALID_ARGUMENT;
    }
    GPIO_Status status = GPIO_ValidatePin(pin, mode, pull, NULL);
    if (status != GPIO_OK) {
        return status;
    }
    if (s_state[pin].mode != (GPIO_Mode)MODE_UNINITIALIZED) {
        return GPIO_ERR_ALREADY_INITIALIZED;
    }

    uint64_t mask = bit64(pin);
    GPIO_Pull effective_pull = pull_for_mode(mode, pull);
    GPIO_HW_SelectGpioFunction(pin);

    if (mode == GPIO_MODE_OUTPUT) {
        /* Latch the level first, keep the driver disabled, then enable input
         * for read-back, and only then enable the output driver. This ordering
         * prevents a LOW glitch on active-low relay boards. */
        GPIO_HW_SetOutputMatrixGpio(pin);
        GPIO_HW_WriteOutputMask(initial_level == GPIO_LEVEL_HIGH ? mask : 0,
                                initial_level == GPIO_LEVEL_HIGH ? 0 : mask);
        GPIO_HW_SetPull(pin, false, false);
        GPIO_HW_SetInputEnable(pin, true);
        GPIO_HW_SetOutputEnableMask(mask, 0);
        s_state[pin].mode = GPIO_MODE_OUTPUT;
        s_state[pin].pull = GPIO_PULL_NONE;
        return GPIO_OK;
    }

    GPIO_HW_SetOutputEnableMask(0, mask);
    GPIO_HW_SetInputEnable(pin, true);
    GPIO_HW_SetPull(pin, effective_pull == GPIO_PULL_UP,
                    effective_pull == GPIO_PULL_DOWN);
    s_state[pin].mode = mode;
    s_state[pin].pull = effective_pull;
    return GPIO_OK;
}

GPIO_Status GPIO_InitWithLevel(uint8_t pin, GPIO_Mode mode, GPIO_Level initial_level)
{
    return GPIO_InitFull(pin, mode, GPIO_PULL_NONE, initial_level);
}

GPIO_Status GPIO_Init(uint8_t pin, GPIO_Mode mode)
{
    return GPIO_InitFull(pin, mode, GPIO_PULL_NONE, GPIO_LEVEL_LOW);
}

GPIO_Status GPIO_Write(uint8_t pin, GPIO_Level level)
{
    ensure_initialized();
    if (pin >= PIN_COUNT) {
        return GPIO_ERR_INVALID_PIN;
    }
    if (s_state[pin].mode == (GPIO_Mode)MODE_UNINITIALIZED) {
        return GPIO_ERR_NOT_INITIALIZED;
    }
    if (s_state[pin].mode != GPIO_MODE_OUTPUT) {
        return GPIO_ERR_NOT_OUTPUT;
    }
    if (level != GPIO_LEVEL_LOW && level != GPIO_LEVEL_HIGH) {
        return GPIO_ERR_INVALID_ARGUMENT;
    }
    uint64_t mask = bit64(pin);
    GPIO_HW_WriteOutputMask(level == GPIO_LEVEL_HIGH ? mask : 0,
                            level == GPIO_LEVEL_HIGH ? 0 : mask);
    return GPIO_OK;
}

GPIO_Status GPIO_Toggle(uint8_t pin)
{
    ensure_initialized();
    if (pin >= PIN_COUNT) {
        return GPIO_ERR_INVALID_PIN;
    }
    if (s_state[pin].mode == (GPIO_Mode)MODE_UNINITIALIZED) {
        return GPIO_ERR_NOT_INITIALIZED;
    }
    if (s_state[pin].mode != GPIO_MODE_OUTPUT) {
        return GPIO_ERR_NOT_OUTPUT;
    }

    bool high;
    if (pin < 32u) {
        high = (GPIO_HW_ReadOutputBits() & (1u << pin)) != 0;
    } else {
        high = (GPIO_HW_ReadOutputBitsHigh() & (1u << (pin - 32u))) != 0;
    }
    uint64_t mask = bit64(pin);
    GPIO_HW_WriteOutputMask(high ? 0 : mask, high ? mask : 0);
    return GPIO_OK;
}

GPIO_Status GPIO_Read(uint8_t pin, uint8_t *value)
{
    ensure_initialized();
    if (value == NULL) {
        return GPIO_ERR_INVALID_ARGUMENT;
    }
    if (pin >= PIN_COUNT) {
        return GPIO_ERR_INVALID_PIN;
    }
    if (s_state[pin].mode == (GPIO_Mode)MODE_UNINITIALIZED) {
        return GPIO_ERR_NOT_INITIALIZED;
    }

    bool high;
    if (s_state[pin].mode == GPIO_MODE_OUTPUT) {
        /* Read the output latch: this is what the pad should be driving. */
        high = pin < 32u
                   ? (GPIO_HW_ReadOutputBits() & (1u << pin)) != 0
                   : (GPIO_HW_ReadOutputBitsHigh() & (1u << (pin - 32u))) != 0;
    } else {
        high = pin < 32u
                   ? (GPIO_HW_ReadInputBits() & (1u << pin)) != 0
                   : (GPIO_HW_ReadInputBitsHigh() & (1u << (pin - 32u))) != 0;
    }
    *value = high ? 1u : 0u;
    return GPIO_OK;
}

GPIO_Status GPIO_SetPull(uint8_t pin, GPIO_Pull pull)
{
    ensure_initialized();
    if (pin >= PIN_COUNT) {
        return GPIO_ERR_INVALID_PIN;
    }
    if (s_state[pin].mode == (GPIO_Mode)MODE_UNINITIALIZED) {
        return GPIO_ERR_NOT_INITIALIZED;
    }
    if (s_state[pin].mode == GPIO_MODE_OUTPUT) {
        return GPIO_ERR_NOT_INPUT;
    }
    if (!valid_pull(pull)) {
        return GPIO_ERR_INVALID_ARGUMENT;
    }
    GPIO_Capabilities caps;
    GPIO_Status status = GPIO_GetCapabilities(pin, &caps);
    if (status != GPIO_OK) {
        return status;
    }
    if (pull == GPIO_PULL_UP && !caps.pullup_capable) {
        return GPIO_ERR_UNSUPPORTED_PULL;
    }
    if (pull == GPIO_PULL_DOWN && !caps.pulldown_capable) {
        return GPIO_ERR_UNSUPPORTED_PULL;
    }
    GPIO_HW_SetPull(pin, pull == GPIO_PULL_UP, pull == GPIO_PULL_DOWN);
    s_state[pin].pull = pull;
    s_state[pin].mode = pull == GPIO_PULL_UP
                            ? GPIO_MODE_INPUT_PULLUP
                            : (pull == GPIO_PULL_DOWN ? GPIO_MODE_INPUT_PULLDOWN
                                                      : GPIO_MODE_INPUT);
    return GPIO_OK;
}

GPIO_Status GPIO_DeInit(uint8_t pin)
{
    ensure_initialized();
    if (pin >= PIN_COUNT) {
        return GPIO_ERR_INVALID_PIN;
    }
    if (s_state[pin].mode == (GPIO_Mode)MODE_UNINITIALIZED) {
        return GPIO_OK;
    }
    uint64_t mask = bit64(pin);
    GPIO_HW_SetOutputEnableMask(0, mask);
    GPIO_HW_SetInputEnable(pin, false);
    GPIO_HW_SetPull(pin, false, false);
    GPIO_HW_SelectGpioFunction(pin);
    s_state[pin].mode = (GPIO_Mode)MODE_UNINITIALIZED;
    s_state[pin].pull = GPIO_PULL_NONE;
    return GPIO_OK;
}

GPIO_Status GPIO_WriteMasks(uint64_t set_mask, uint64_t clear_mask)
{
    ensure_initialized();
    if ((set_mask & clear_mask) != 0) {
        return GPIO_ERR_CONFLICT;
    }
    uint64_t all = set_mask | clear_mask;
    if ((all >> PIN_COUNT) != 0) {
        return GPIO_ERR_INVALID_PIN;
    }
    for (uint8_t pin = 0; pin < PIN_COUNT; ++pin) {
        if ((all & bit64(pin)) == 0) {
            continue;
        }
        if (s_state[pin].mode == (GPIO_Mode)MODE_UNINITIALIZED) {
            return GPIO_ERR_NOT_INITIALIZED;
        }
        if (s_state[pin].mode != GPIO_MODE_OUTPUT) {
            return GPIO_ERR_NOT_OUTPUT;
        }
    }
    GPIO_HW_WriteOutputMask(set_mask, clear_mask);
    return GPIO_OK;
}

GPIO_Status GPIO_WriteMask(uint64_t set_mask, uint64_t clear_mask)
{
    return GPIO_WriteMasks(set_mask, clear_mask);
}

GPIO_Status GPIO_GetMode(uint8_t pin, GPIO_Mode *mode)
{
    ensure_initialized();
    if (mode == NULL) {
        return GPIO_ERR_INVALID_ARGUMENT;
    }
    if (pin >= PIN_COUNT) {
        return GPIO_ERR_INVALID_PIN;
    }
    if (s_state[pin].mode == (GPIO_Mode)MODE_UNINITIALIZED) {
        return GPIO_ERR_NOT_INITIALIZED;
    }
    *mode = s_state[pin].mode;
    return GPIO_OK;
}

GPIO_Status GPIO_GetPull(uint8_t pin, GPIO_Pull *pull)
{
    ensure_initialized();
    if (pull == NULL) {
        return GPIO_ERR_INVALID_ARGUMENT;
    }
    if (pin >= PIN_COUNT) {
        return GPIO_ERR_INVALID_PIN;
    }
    if (s_state[pin].mode == (GPIO_Mode)MODE_UNINITIALIZED) {
        return GPIO_ERR_NOT_INITIALIZED;
    }
    *pull = s_state[pin].pull;
    return GPIO_OK;
}

const char *GPIO_StatusName(GPIO_Status status)
{
    switch (status) {
    case GPIO_OK: return "ok";
    case GPIO_ERR_INVALID_PIN: return "invalid-pin";
    case GPIO_ERR_RESERVED: return "board-blocked";
    case GPIO_ERR_UNSUPPORTED_MODE: return "unsupported-mode";
    case GPIO_ERR_UNSUPPORTED_PULL: return "unsupported-pull";
    case GPIO_ERR_ALREADY_INITIALIZED: return "already-initialized";
    case GPIO_ERR_NOT_INITIALIZED: return "not-initialized";
    case GPIO_ERR_NOT_OUTPUT: return "not-output";
    case GPIO_ERR_NOT_INPUT: return "not-input";
    case GPIO_ERR_INVALID_ARGUMENT: return "invalid-argument";
    case GPIO_ERR_CONFLICT: return "conflict";
    case GPIO_ERR_HARDWARE: return "hardware";
    default: return "unknown";
    }
}

const char *GPIO_ModeName(GPIO_Mode mode)
{
    switch (mode) {
    case GPIO_MODE_INPUT: return "input";
    case GPIO_MODE_OUTPUT: return "output";
    case GPIO_MODE_INPUT_PULLUP: return "input-pullup";
    case GPIO_MODE_INPUT_PULLDOWN: return "input-pulldown";
    default: return "uninitialized";
    }
}

const char *GPIO_PullName(GPIO_Pull pull)
{
    switch (pull) {
    case GPIO_PULL_NONE: return "none";
    case GPIO_PULL_UP: return "up";
    case GPIO_PULL_DOWN: return "down";
    default: return "unknown";
    }
}

#ifdef GPIO_DRIVER_HOST_TEST
void GPIO_TestResetDriverState(void)
{
    for (uint8_t pin = 0; pin < PIN_COUNT; ++pin) {
        s_state[pin].mode = (GPIO_Mode)MODE_UNINITIALIZED;
        s_state[pin].pull = GPIO_PULL_NONE;
    }
    s_initialized = true;
}
#endif
