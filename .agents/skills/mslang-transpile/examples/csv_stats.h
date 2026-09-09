#ifndef CSV_STATS_H
#define CSV_STATS_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reads the CSV file at `path`, sums the numeric field of every
 * `name,value` row, and prints `sum=<value>` to `out`.
 * Returns 0 on success, -1 on any failure.
 */
int32_t CsvStatsRun(const char *path, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* CSV_STATS_H */
