#ifndef _VERSION_H
#define _VERSION_H

#include <stdint.h>

constexpr uint16_t MAJOR = 3;
constexpr uint16_t MINOR = 5;
constexpr uint16_t PATCH = 5;

#define FI_version MAJOR * 100 + MINOR * 10 + PATCH

#endif