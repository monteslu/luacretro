/* luacretro runtime: the platform layer an SDK implements.
 *
 * This is the whole seam between the portable runtime and a console. The
 * runtime keeps PICO-8's 128x128 4bpp screen, draw state and memory map in
 * its own RAM; the platform only shows frames, reads pads, plays samples
 * and stores the 256-byte cartdata block.
 */
#ifndef LC_PLAT_H
#define LC_PLAT_H
#include <stdint.h>
#include <stddef.h>

/* Show a finished frame and wait for the next vertical blank.
 *   screen:  0x2000 bytes, PICO-8 layout (2 pixels per byte, low nibble left)
 *   pal:     16 bytes, PICO-8 screen palette (0x5f10): each entry 0-15 or
 *            128-143 for the extended colors
 *   mode:    PICO-8 screen mode byte (0x5f2c)
 * Called once per 60 Hz tick; a 30 fps cart presents every other tick. */
void lc_plat_present(const uint8_t *screen, const uint8_t *pal, uint8_t mode);

/* Wait one 60 Hz tick without a new frame (30 fps carts). */
void lc_plat_vsync(void);

/* Pad state for player 0..7 as PICO-8 button bits: 0 left, 1 right, 2 up,
 * 3 down, 4 O, 5 X, 6 pause. */
uint8_t lc_plat_buttons(int player);

/* Audio: the runtime synthesizes 22050 Hz mono signed 16-bit samples. The
 * platform pulls them with lc_p8_audio_fill() at whatever rate it likes and
 * resamples if its output differs. */

/* cartdata persistence (64 numbers = 256 bytes). Return 1 if loaded. */
int lc_plat_cartdata_load(const char *id, uint8_t *data256);
void lc_plat_cartdata_save(const char *id, const uint8_t *data256);

/* Seed for the RNG at cart start (PICO-8 seeds from the clock; a fixed
 * value makes runs reproducible). */
uint32_t lc_plat_seed(void);

/* Wall clock for stat(80..85) (UTC) and stat(90..95) (local): year, month,
 * day, hour, minute, second. A platform without a clock returns a fixed date. */
void lc_plat_datetime(int out[6]);

/* printh() */
void lc_plat_log(const char *s, size_t len);

/* Unrecoverable runtime error on the main thread: show it and stop. */
_Noreturn void lc_plat_fatal(const char *msg);

#endif
