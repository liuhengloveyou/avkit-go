#ifndef METADATA_H
#define METADATA_H

#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>

typedef struct metaData {
    int width;
    int height;
    int64_t bit_rate;
    int64_t duration;
    double totle_seconds;
    char fn[512];
} metaData_t;

metaData_t *get_metadata (const char *fn);

#endif