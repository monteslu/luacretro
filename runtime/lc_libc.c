/* luacretro runtime: the few libc routines the runtime needs.
 *
 * The runtime never includes a libc header, so it links on freestanding
 * platforms. GCC may still emit calls to memcpy/memset/memmove/memcmp for
 * struct copies and large initialisers; weak definitions are provided for
 * those, which a platform libc overrides where one exists.
 */
#include <stddef.h>
#include <stdint.h>

void *lc_memcpy(void *d, const void *s, size_t n) {
  uint8_t *dp = (uint8_t *)d;
  const uint8_t *sp = (const uint8_t *)s;
  if ((((uintptr_t)dp | (uintptr_t)sp) & 3) == 0) {
    while (n >= 4) { *(uint32_t *)dp = *(const uint32_t *)sp; dp += 4; sp += 4; n -= 4; }
  }
  while (n--) *dp++ = *sp++;
  return d;
}

void *lc_memset(void *d, int c, size_t n) {
  uint8_t *dp = (uint8_t *)d;
  if (((uintptr_t)dp & 3) == 0 && n >= 4) {
    uint32_t w = (uint8_t)c;
    w |= w << 8;
    w |= w << 16;
    while (n >= 4) { *(uint32_t *)dp = w; dp += 4; n -= 4; }
  }
  while (n--) *dp++ = (uint8_t)c;
  return d;
}

int lc_memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
  for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  return 0;
}

size_t lc_strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

#ifndef LC_HOSTED_LIBC
__attribute__((weak)) void *memcpy(void *d, const void *s, size_t n) { return lc_memcpy(d, s, n); }
__attribute__((weak)) void *memset(void *d, int c, size_t n) { return lc_memset(d, c, n); }
__attribute__((weak)) int memcmp(const void *a, const void *b, size_t n) { return lc_memcmp(a, b, n); }
__attribute__((weak)) void *memmove(void *d, const void *s, size_t n) {
  uint8_t *dp = (uint8_t *)d;
  const uint8_t *sp = (const uint8_t *)s;
  if (dp < sp) { while (n--) *dp++ = *sp++; }
  else { dp += n; sp += n; while (n--) *--dp = *--sp; }
  return d;
}
#endif
