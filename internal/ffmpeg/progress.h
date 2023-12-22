#pragma once

#include <stdint.h>

// 进度回调函数指针
typedef int (*ProgressCBPtr)(char *t, char *fn, double duration, double pos);

// 设置压缩任务进度回调函数指针
void setTranscodeProgressFunc(ProgressCBPtr f);
