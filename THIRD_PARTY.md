# Third-party notices

luacretro is MIT licensed (see LICENSE). The dynamic tier's runtime
(`runtime/`) adapts code and algorithms from the projects below.

| Project | Used for | License |
|---|---|---|
| [FAKE-08](https://github.com/jtothebell/fake-08) | the PICO-8 machine: graphics, print, synth, memory, input (`lc_p8*.c`) | MIT |
| [zepto8](https://github.com/samhocevar/zepto8) | PICO-8 behaviors FAKE-08 inherits (synth, fixed-point semantics) | WTFPL 2 |
| [tac08](https://github.com/0xcafed00d/tac08) | drawing routines FAKE-08 inherits | MIT |
| [z8lua](https://github.com/samhocevar/z8lua) | PICO-8 number semantics, number hash, lexer rules | MIT |
| [Lua 5.2](https://www.lua.org/) | table implementation (`lc_tab.c`, from `ltable.c`) | MIT |
| PICO-8 font (Lexaloffle) | `lc_p8_font` in `lc_p8print.c` | CC0 |
| [Stack Overflow answer 8448181](https://stackoverflow.com/a/8448181) | the ellipse algorithm behind `oval`/`ovalfill`, via FAKE-08 | CC BY-SA 3.0 |

PICO-8 is a product of Lexaloffle Games. luacretro is not affiliated with or
endorsed by Lexaloffle.

## MIT license (FAKE-08, tac08, z8lua)

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Lua

```
Copyright (C) 1994-2015 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## WTFPL 2 (zepto8)

```
DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
Version 2, December 2004

Everyone is permitted to copy and distribute verbatim or modified
copies of this license document, and changing it is allowed as long
as the name is changed.

DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE
TERMS AND CONDITIONS FOR COPYING, DISTRIBUTION AND MODIFICATION

0. You just DO WHAT THE FUCK YOU WANT TO.
```
