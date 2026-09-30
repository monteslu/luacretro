/* luacretro dynamic tier: headless host platform for tests.
 *
 * Runs a compiled cart for a fixed number of 60 Hz ticks and records what a
 * console would show: every tick's screen (0x2000 bytes, PICO-8 layout) and
 * screen palette go to the dump file, printh() goes to stdout. Buttons come
 * from an input script ("<tick> <mask>" lines, mask held from that tick on).
 * Built non-PIE with the heap mapped below 4 GiB, as the runtime requires.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include "lc.h"
#include "lc_p8.h"
#include "lc_plat.h"

static long max_ticks = 60, tick;
static FILE *dump;
static long in_tick[4096];
static uint8_t in_mask[4096];
static int n_in;
static uint8_t cur_mask;

static void end_tick(void) {
  tick++;
  for (int i = 0; i < n_in; i++) if (in_tick[i] == tick) cur_mask = in_mask[i];
  if (tick >= max_ticks) {
    if (getenv("LC_MEMSTAT")) {
      extern void lc_gc_census(size_t *, size_t *);
      size_t b[8], c[8];
      lc_gc_collect();
      lc_gc_census(b, c);
      fprintf(stderr, "heap: live %zu, peak alloc %zu, peak live %zu | str %zu/%zu tab %zu/%zu clo %zu/%zu cell %zu/%zu thr %zu raw %zu/%zu vec %zu\n",
        lc_gc_used(), lc_gc_peak(), lc_gc_peak_live(), b[1], c[1], b[2], c[2], b[3], c[3], b[4], c[4], b[5], b[6], c[6], b[7]);
    }
    if (dump) fclose(dump);
    fflush(stdout);
    exit(0);
  }
}

static void record(void) {
  if (!dump) return;
  fwrite(lc_p8_screenbuf(), 1, 0x2000, dump);
  fwrite(lc_p8mem + P8_SCREENPAL, 1, 16, dump);
}

void lc_plat_present(const uint8_t *screen, const uint8_t *pal, uint8_t mode) {
  (void)screen; (void)pal; (void)mode;
  record();
  end_tick();
}
void lc_plat_vsync(void) { record(); end_tick(); }
uint8_t lc_plat_buttons(int player) { return player == 0 ? cur_mask : 0; }
int lc_plat_cartdata_load(const char *id, uint8_t *d) { (void)id; (void)d; return 0; }
void lc_plat_cartdata_save(const char *id, const uint8_t *d) { (void)id; (void)d; }
uint32_t lc_plat_seed(void) { return 0; }
void lc_plat_log(const char *s, size_t len) { fwrite(s, 1, len, stdout); fputc('\n', stdout); }
_Noreturn void lc_plat_fatal(const char *msg) {
  fprintf(stdout, "RUNTIME ERROR: %s\n", msg);
  fflush(stdout);
  if (dump) fclose(dump);
  exit(2);
}

static lv vstack[16384];

static void on_alarm(int sig) {
  (void)sig;
  fprintf(stdout, "TIMEOUT at tick %ld, line %d\n", tick, lc_curline);
  fflush(stdout);
  _exit(3);
}

int main(int argc, char **argv) {
  char base;
  if (getenv("LC_TIMEOUT")) { signal(SIGALRM, on_alarm); alarm((unsigned)atoi(getenv("LC_TIMEOUT"))); }
  if (argc > 1) max_ticks = atol(argv[1]);
  if (argc > 2 && strcmp(argv[2], "-") != 0) dump = fopen(argv[2], "wb");
  if (argc > 3) {
    FILE *f = fopen(argv[3], "r");
    if (f) {
      long t; unsigned m;
      while (n_in < 4096 && fscanf(f, "%ld %u", &t, &m) == 2) { in_tick[n_in] = t; in_mask[n_in] = (uint8_t)m; n_in++; }
      fclose(f);
      for (int i = 0; i < n_in; i++) if (in_tick[i] == 0) cur_mask = in_mask[i];
    }
  }
  size_t asz = argc > 4 ? (size_t)atol(argv[4]) : (size_t)512 * 1024;
  void *arena = mmap(NULL, asz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (arena == MAP_FAILED) { perror("mmap"); return 1; }
  lc_p8_run(arena, asz, vstack, sizeof vstack / sizeof vstack[0], &base, (size_t)4 << 20);
}
