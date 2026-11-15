#ifndef LOG_FILE_CAP_H
#define LOG_FILE_CAP_H

#include <stdio.h>
#include <unistd.h>

/* The cap every camera log on the printer shares. Included by a path relative to the including
 * file, so each program builds from it without an include option of its own. */
#define LOG_MAX_BYTES (256 * 1024)

/* Emptied rather than rotated: a camera fault repeating thirty times a second would otherwise fill
 * the printer's flash. A stream that cannot say how long it is, such as a terminal, is left be. */
static inline void log_empty_when_full(FILE *stream)
{
    long bytes_written = ftell(stream);

    if (bytes_written < LOG_MAX_BYTES)
        return;
    if (ftruncate(fileno(stream), 0) != 0)
        return;
    rewind(stream);
}

#endif /* LOG_FILE_CAP_H */
