// The SDL3 front end (front_sdl.c) as the window thread sees it (vk_window.c), and the lock that serialises SDL's subsystem start and stop
// (SDL_Init / SDL_InitSubSystem are not thread safe: the window thread starts video and controllers, an audio output may start audio at the same time).
#pragma once
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void front_sdl_start(SDL_Window *win);      // on the window thread, once the window exists and the controller subsystem is up
void front_sdl_event(const SDL_Event *e);   // every event the window thread pumps
void front_sdl_tick(void);                  // every turn of the window thread's loop (at least every 50 ms): rumble kept going, the controller check's title
void front_sdl_lock(void);
void front_sdl_unlock(void);

#ifdef __cplusplus
}
#endif
