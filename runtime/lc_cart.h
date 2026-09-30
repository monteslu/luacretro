/* luacretro runtime: what the compiler's generated C provides. */
#ifndef LC_CART_H
#define LC_CART_H
#include "lc.h"

enum { LC_CB_INIT, LC_CB_UPDATE, LC_CB_UPDATE60, LC_CB_DRAW };

/* 0x4300 bytes: sprite sheet, map, flags, music, sfx */
extern const uint8_t lc_cart_rom[];

/* Reset every global to its initial state and intern the string literals;
 * called on each (re)start after the heap is initialised. */
void lc_cart_setup(void);

/* Run the cart's top-level code. */
void lc_cart_main(void);

/* Current value of _init / _update / _update60 / _draw. */
lv lc_cart_cb(int which);

#endif
