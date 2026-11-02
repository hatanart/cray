/* cray-sdl: desktop window for trying the C emulator on a PC.
 * Same keys as the Pi build; the 23 lamp lines are drawn under the picture.
 *   cray-sdl [-r romdir] [-R rom] [-n nvram] [-s scale] [-q]  (-q = no sound)
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ray.h"
#include "session.h"

#define LAMP_H 24

static SDL_AudioDeviceID audio_dev;

static int key_button(SDL_Keycode k)
{
    switch (k) {
    case SDLK_1: return BTN_HOLD1;
    case SDLK_2: return BTN_HOLD2;
    case SDLK_3: return BTN_HOLD3;
    case SDLK_4: return BTN_HOLD4;
    case SDLK_5: return BTN_HOLD5;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return BTN_DEAL;
    case SDLK_b: return BTN_BET;
    case SDLK_p: return BTN_COLLECT;
    case SDLK_d: return BTN_DOUBLE;
    case SDLK_LEFT: return BTN_SMALL;
    case SDLK_RIGHT: return BTN_BIG;
    case SDLK_s: return BTN_SERVICE;
    case SDLK_F1: return BTN_SRVC1;
    case SDLK_F2: return BTN_SRVC2;
    case SDLK_F3: return BTN_K13;
    case SDLK_F4: return BTN_K14;
    case SDLK_F5: return BTN_K15;
    case SDLK_F6: return BTN_K16;
    default: return -1;
    }
}

int main(int argc, char **argv)
{
    SessionOpts o;
    session_defaults(&o);
    int scale = 2, opt;
    while ((opt = getopt(argc, argv, "r:R:n:s:qb:")) != -1) {
        switch (opt) {
        case 'r': o.romdir = optarg; break;
        case 'R': o.rom = optarg; break;
        case 'n': o.nvram = optarg; break;
        case 's': scale = atoi(optarg) > 0 ? atoi(optarg) : 2; break;
        case 'q': o.sound = 0; break;
        case 'b': o.board = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: cray-sdl [-r romdir] [-R rom] [-n nvram] [-s scale] [-q] [-b 5501|5502]\n");
            return 2;
        }
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    if (o.sound) {
        SDL_AudioSpec want = { .freq = AUDIO_RATE, .format = AUDIO_F32SYS, .channels = 1, .samples = 512 }, have;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!audio_dev) {
            fprintf(stderr, "no sound: %s\n", SDL_GetError());
            o.sound = 0;
        } else {
            SDL_PauseAudioDevice(audio_dev, 0);
        }
    }
    if (session_open(&o, NULL))
        return 1;
    char title[300];
    snprintf(title, sizeof title, "RAY - %s", session_title());
    SDL_Window *win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       SCREEN_W * scale, SCREEN_H * scale + LAMP_H, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, SCREEN_W * scale, SCREEN_H * scale + LAMP_H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                         SCREEN_W, SCREEN_H);
    static uint8_t img[SCREEN_W * SCREEN_H];
    static uint32_t px[SCREEN_W * SCREEN_H];
    static float audio[AUDIO_RATE / TICK_HZ + 8];
    uint64_t freq = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter();
    long long done = 0;
    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) && !ev.key.repeat) {
                int down = ev.type == SDL_KEYDOWN;
                SDL_Keycode k = ev.key.keysym.sym;
                int b = key_button(k);
                if (b >= 0) ray_set_button(b, down);
                else if (down && k == SDLK_i) session_coin();
                else if (down && k == SDLK_F10) session_toggle_door();
                else if (down && k == SDLK_F9) session_refill();
                else if (down && k == SDLK_ESCAPE) running = 0;
            }
        }
        long long target = (long long)((SDL_GetPerformanceCounter() - t0) * TICK_HZ / freq);
        if (target - done > 20) done = target - 2;
        for (int i = 0; i < 4 && done < target; i++, done++) {
            int n = session_tick(audio);
            if (audio_dev && n) {
                /* keep about 60 ms queued; drop if far behind */
                if (SDL_GetQueuedAudioSize(audio_dev) < AUDIO_RATE * sizeof(float) / 4)
                    SDL_QueueAudio(audio_dev, audio, n * sizeof(float));
            }
        }
        uint32_t pal[16];
        video_render(&ray.acrtc, img);
        ray_palette(0, pal);
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
            px[i] = 0xFF000000u | pal[img[i]];
        SDL_UpdateTexture(tex, NULL, px, SCREEN_W * 4);
        SDL_SetRenderDrawColor(ren, 24, 24, 28, 255);
        SDL_RenderClear(ren);
        SDL_Rect dst = { 0, 0, SCREEN_W * scale, SCREEN_H * scale };
        SDL_RenderCopy(ren, tex, NULL, &dst);
        uint32_t lamps = ray_lamps();
        for (int b = 0; b < 23; b++) {
            int on = (lamps >> b) & 1;
            SDL_SetRenderDrawColor(ren, on ? 255 : 60, on ? 210 : 55, on ? 60 : 40, 255);
            SDL_Rect r = { 8 + b * 22, SCREEN_H * scale + 6, 14, 12 };
            SDL_RenderFillRect(ren, &r);
        }
        SDL_RenderPresent(ren);
    }
    session_shutdown();
    SDL_Quit();
    return 0;
}
