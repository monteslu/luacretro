/* luacretro runtime: a silent PICO-8 audio engine.
 *
 * Same API as lc_p8snd.c for targets that cannot afford synthesis (or while
 * a platform's audio path is being brought up): sfx() and music() are
 * accepted and do nothing, stat() reports nothing playing. An SDK compiles
 * this INSTEAD of lc_p8snd.c.
 */
#include "lc_p8.h"

void lc_p8_audio_init(void) {}
void lc_p8_sfx(int n, int channel, int offset, int length) { (void)n; (void)channel; (void)offset; (void)length; }
void lc_p8_music(int n, int fade_ms, int mask) { (void)n; (void)fade_ms; (void)mask; }
int lc_p8_audio_stat(int n) { return (n >= 20 && n <= 23) || (n >= 50 && n <= 53) ? -1 : -1; }
void lc_p8_audio_fill(int16_t *out, int n) { for (int i = 0; i < n; i++) out[i] = 0; }
