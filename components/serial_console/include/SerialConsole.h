#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deterministic serial console.
 *
 * Natural-language text is only handled by the "do" and "classify" commands.
 * Everything else is a local, tokenized management command. The same command
 * parser is used by host tests through serial_console_execute_line().
 */

typedef void (*serial_console_output_fn)(void *context, const char *text);

/* Starts the UART0 console task (ESP-IDF target only). Returns 0 on success. */
int serial_console_start(void);

/* Executes one line. If out is NULL, writes to stdout. Returns 0 when the
 * command was recognized, -1 for an empty/unknown command. */
int serial_console_execute_line(const char *line, serial_console_output_fn out,
                                void *context);

#ifdef __cplusplus
}
#endif
