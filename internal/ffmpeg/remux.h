#ifndef REMUX_H
#define REMUX_H

#include "progress.h"

typedef struct Remuxer
{
    char in_file[1024];
    char out_file[1024];

    double duration;
    double pos;

    ProgressCBPtr progressCB;
} Remuxer;

Remuxer *newRemuxer(const char *in, const char *out, ProgressCBPtr cb);
int remux(Remuxer *remuxer);

#endif