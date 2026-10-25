#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Explicit pin-configuration persistence.
 *
 * A saved blob is an envelope around the versioned pin_manager export:
 *   magic[4] = "TACF", u16 envelope version, u16 schema version,
 *   u32 payload length, u32 CRC32(payload), payload.
 *
 * Nothing is written during normal operation. The operator must issue
 * "config save". At boot the blob is loaded, CRC-checked, schema-checked and
 * fully revalidated by pin_manager before any output is initialized. Imported
 * outputs are always initialized OFF; logical ON state is never restored.
 */

typedef enum {
    CFG_OK = 0,
    /* Pin configuration loaded and validated, but the sensor or rule section
     * was rejected. The pin set stays active; sensors and rules are empty. */
    CFG_PARTIAL,
    CFG_NOT_FOUND,
    CFG_INVALID,
    CFG_UNSUPPORTED,
    CFG_IO_ERROR,
    CFG_NO_SPACE,
} config_store_status_t;

config_store_status_t config_store_save(void);
config_store_status_t config_store_load(void);
config_store_status_t config_store_erase(void);
config_store_status_t config_store_probe(void);
const char *config_store_status_name(config_store_status_t status);
const char *config_store_last_detail(void);

#ifdef __cplusplus
}
#endif
