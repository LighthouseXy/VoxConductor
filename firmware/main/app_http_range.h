#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// One RFC byte range only. Overflow, whitespace and multipart ranges are rejected.
static bool app_range_number(const char **p, uint64_t *out) {
  const char *s = *p;
  if (*s < '0' || *s > '9') return false;
  uint64_t value = 0;
  while (*s >= '0' && *s <= '9') {
    unsigned digit = (unsigned)(*s++ - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *p = s; *out = value;
  return true;
}
static bool app_http_range(const char *header, uint64_t size,
                           uint64_t *start, uint64_t *end) {
  if (!header || !size || strncmp(header, "bytes=", 6)) return false;
  const char *p = header + 6;
  uint64_t a = 0, b = 0;
  if (*p == '-') {
    ++p;
    if (!app_range_number(&p, &b) || *p || !b) return false;
    a = b < size ? size - b : 0;
    b = size - 1;
  } else {
    if (!app_range_number(&p, &a) || *p++ != '-' || a >= size) return false;
    if (!*p) b = size - 1;
    else {
      if (!app_range_number(&p, &b) || *p || b < a) return false;
      if (b >= size) b = size - 1;
    }
  }
  *start = a; *end = b;
  return true;
}
