#pragma once

#include <unistd.h>

inline int checked_chdir(const char* path) {
    const int rc = ::chdir(path);
    if (rc != 0) {
        _exit(127);
    }
    return rc;
}

#define chdir(path) checked_chdir(path)
