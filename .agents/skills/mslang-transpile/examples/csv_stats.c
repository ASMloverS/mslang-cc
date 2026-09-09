#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <msl/io.h>
#include <msl/math.h>
#include <msl/string.h>

#include "csv_stats.h"

/*--- Types ---------------------------------------------------------------*/

typedef struct CsvStats
{
    char    *name;
    uint64_t total;
    uint32_t count;
} CsvStats;

/*--- Functions ------------------------------------------------------------*/

static bool CsvStatsIsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char *CsvStatsTrim(char *text)
{
    char *end;

    while (CsvStatsIsSpace(*text))
    {
        text++;
    }

    end = text + strlen(text);
    while (end > text && CsvStatsIsSpace(end[-1]))
    {
        *--end = '\0';
    }

    return text;
}

/*
 * Splits `line` in place at the first comma.
 * Returns the right field; the left field stays in `line`.
 */
static char *CsvStatsSplit(char *line)
{
    char *comma = strchr(line, ',');

    if (comma == NULL)
    {
        return NULL;
    }

    *comma = '\0';
    return comma + 1;
}

static bool CsvStatsParseU64(const char *text, uint64_t *out)
{
    uint64_t value = 0;
    bool     have  = false;

    while (*text >= '0' && *text <= '9')
    {
        if (value > (UINT64_MAX - 9u) / 10u)
        {
            return false;
        }
        value = value * 10u + MslCastU32ToU64((uint32_t)(*text - '0'));
        have  = true;
        text++;
    }

    if (!have || *text != '\0')
    {
        return false;
    }

    *out = value;
    return true;
}

bool CsvStatsParseLine(char *line, CsvStats *out)
{
    char *left;
    char *right;
    char *name;

    if (line == NULL || out == NULL)
    {
        return false;
    }

    right = CsvStatsSplit(line);
    if (right == NULL)
    {
        return false;
    }

    left  = CsvStatsTrim(line);
    right = CsvStatsTrim(right);

    if (*left == '\0')
    {
        return false;
    }

    name = msl_string_duplicate(left);
    if (name == NULL)
    {
        return false;
    }

    if (!CsvStatsParseU64(right, &out->total))
    {
        free(name);
        return false;
    }

    out->name  = name;
    out->count = 1;
    return true;
}

void CsvStatsDestroy(CsvStats *self)
{
    if (self == NULL)
    {
        return;
    }

    free(self->name);
    self->name = NULL;
}

int32_t CsvStatsRun(const char *path, FILE *out)
{
    MslString content;
    CsvStats  stats    = { 0 };
    char     *cursor   = NULL;
    char     *line     = NULL;
    uint64_t  sum      = 0;
    int32_t   result   = -1;

    if (path == NULL || out == NULL)
    {
        return -1;
    }

    if (msl_io_read_file(&content, path) != MSL_IO_ERROR_OK)
    {
        return -1;
    }

    while ((line = msl_string_tokenize(content.data, &cursor, "\n")) != NULL)
    {
        if (!CsvStatsParseLine(line, &stats))
        {
            goto cleanup;
        }

        sum += stats.total;
        CsvStatsDestroy(&stats);
    }

    fprintf(out, "sum=%" PRIu64 "\n", sum);
    result = 0;

cleanup:
    CsvStatsDestroy(&stats);
    msl_string_destroy(&content);
    return result;
}
