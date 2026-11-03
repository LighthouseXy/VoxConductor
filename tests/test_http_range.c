#include "app_http_range.h"
#include <assert.h>
int main(void) {
  uint64_t a, b;
  assert(app_http_range("bytes=0-0", 100, &a, &b) && a == 0 && b == 0);
  assert(app_http_range("bytes=50-", 100, &a, &b) && a == 50 && b == 99);
  assert(app_http_range("bytes=-20", 100, &a, &b) && a == 80 && b == 99);
  assert(app_http_range("bytes=-200", 100, &a, &b) && a == 0 && b == 99);
  assert(app_http_range("bytes=5-999", 100, &a, &b) && a == 5 && b == 99);
  const char *bad[] = {"", "bytes=", "bytes=-0", "bytes=100-", "bytes=20-10",
    "bytes=0-1,4-5", "bytes=0-1x", "bytes= 0-1", "bytes=18446744073709551616-"};
  for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); ++i)
    assert(!app_http_range(bad[i], 100, &a, &b));
  assert(!app_http_range("bytes=0-", 0, &a, &b));
}
