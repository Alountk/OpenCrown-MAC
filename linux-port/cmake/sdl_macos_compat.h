// Force-included when building SDL 3.4.18 on macOS: SDL_cpuinfo.c calls getpagesize(), which SDL's own feature macros
// (_XOPEN_SOURCE=700) hide in the current SDK's <unistd.h>. The function itself is still in libSystem.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int getpagesize(void);
#ifdef __cplusplus
}
#endif
