#pragma once

#include "jd_sdk.h"

#define JDVM_NUM_REGS 16

typedef float value_t;

size_t jdvm_strformat(const char *fmt, size_t fmtlen, char *dst, size_t dstlen, value_t *args, size_t numargs);
