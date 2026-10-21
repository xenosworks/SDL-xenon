/*
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/

/* Checks SDL's xsdk drivers on the Xbox 360: the platform's name, paths, the timer, threads, the controller and
   audio. Each check writes PASS or FAIL to xsdk's log, named after the title: game:\sdl_check.log. */

#include <SDL3/SDL.h>

#include "xsdk/audio.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

#define SPLASH_MS       2000
#define DELAY_MS        100
#define POLL_MS         3000
#define RUMBLE_MS       500
#define TONE_HZ         750
#define TONE_MS         2000
#define TONE_RATE       48000
#define TONE_AMPLITUDE  0.25f
// The console's time base steps in about 20 ns; the kernel's system time, a millisecond at a time.
#define FINEST_STEP_NS  100000
#define STEP_READS      1000000

static int passed_count;
static int failed_count;

static void check(const char *name, bool passed)
{
    xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
    if (passed) {
        passed_count++;
    } else {
        failed_count++;
        xsdk_log_write("    SDL_GetError: %s", SDL_GetError());
    }
}

static void check_paths(void)
{
    const char *base = SDL_GetBasePath();
    xsdk_log_write("sdl: base path %s", base ? base : "(none)");
    check("sdl: the base path is game:\\", base && SDL_strcmp(base, "game:\\") == 0);

    char *pref = SDL_GetPrefPath("xsdk", "sdl_check");
    xsdk_log_write("sdl: pref path %s", pref ? pref : "(none)");
    bool written = false;
    if (pref) {
        char file[256];
        SDL_snprintf(file, sizeof(file), "%scheck.txt", pref);
        static const char text[] = "written through SDL_IOStream";
        SDL_IOStream *out = SDL_IOFromFile(file, "wb");
        if (out) {
            written = SDL_WriteIO(out, text, sizeof(text)) == sizeof(text);
            written = SDL_CloseIO(out) && written;
        }
        char back[sizeof(text)] = { 0 };
        SDL_IOStream *in = written ? SDL_IOFromFile(file, "rb") : NULL;
        written = in && SDL_ReadIO(in, back, sizeof(back)) == sizeof(back) && SDL_memcmp(back, text, sizeof(text)) == 0;
        if (in) {
            SDL_CloseIO(in);
        }
        SDL_free(pref);
    }
    check("sdl: the pref path game:\\xsdk\\sdl_check\\ is made, and a file written there reads back", written);
}

static void check_timer(void)
{
    const Uint64 start = SDL_GetTicksNS();
    SDL_Delay(DELAY_MS);
    const Uint64 delay_us = (SDL_GetTicksNS() - start) / 1000;
    const Uint64 precise_start = SDL_GetTicksNS();
    SDL_DelayPrecise(SDL_MS_TO_NS(16));
    const Uint64 precise_us = (SDL_GetTicksNS() - precise_start) / 1000;
    xsdk_log_write("sdl: SDL_Delay(%d) took %u us; SDL_DelayPrecise(16 ms) took %u us", DELAY_MS, (unsigned)delay_us,
                   (unsigned)precise_us);
    check("sdl: SDL_Delay(100) takes 100 to 150 ms by SDL_GetTicksNS", delay_us >= 100000 && delay_us < 150000);

    // SDL reads CLOCK_MONOTONIC_RAW where the C library defines it, and falls back to gettimeofday if that's refused.
    const Uint64 first = SDL_GetTicksNS();
    Uint64 now = first;
    for (int i = 0; i < STEP_READS && now == first; i++) {
        now = SDL_GetTicksNS();
    }
    xsdk_log_write("sdl: SDL_GetTicksNS steps %u ns; the performance counter runs at %u Hz", (unsigned)(now - first),
                   (unsigned)SDL_GetPerformanceFrequency());
    check("sdl: SDL's clock steps finer than 0.1 ms", now != first && now - first < FINEST_STEP_NS);
}

static int SDLCALL thread_main(void *data)
{
    SDL_SetAtomicInt((SDL_AtomicInt *)data, 1);
    return 7;
}

static void check_thread(void)
{
    SDL_AtomicInt ran;
    SDL_SetAtomicInt(&ran, 0);
    SDL_Thread *thread = SDL_CreateThread(thread_main, "sdl_check", &ran);
    int result = 0;
    if (thread) {
        SDL_WaitThread(thread, &result);
    }
    check("sdl: a thread runs and returns its value", thread && result == 7 && SDL_GetAtomicInt(&ran) == 1);
}

static void log_buttons(SDL_Gamepad *pad, Uint32 *seen)
{
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; button++) {
        const Uint32 bit = 1u << button;
        if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)button) && !(*seen & bit)) {
            *seen |= bit;
            xsdk_log_write("sdl: gamepad button %s pressed",
                           SDL_GetGamepadStringForButton((SDL_GamepadButton)button));
        }
    }
}

static void check_gamepad(void)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    check("sdl: a gamepad is connected", ids && count > 0);
    if (!ids || count == 0) {
        SDL_free(ids);
        return;
    }
    SDL_Gamepad *pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    check("sdl: the gamepad opens", pad != NULL);
    if (!pad) {
        return;
    }
    xsdk_log_write("sdl: gamepad \"%s\", player %d, type %s", SDL_GetGamepadName(pad), SDL_GetGamepadPlayerIndex(pad),
                   SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)));
    check("sdl: SDL takes it for an Xbox 360 controller", SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_XBOX360);

    xsdk_log_write("sdl: reading the gamepad for %d ms; press some buttons and move the sticks", POLL_MS);
    Uint32 seen = 0;
    Sint16 left_x_min = 0, left_x_max = 0;
    const Uint64 end = SDL_GetTicks() + POLL_MS;
    while (SDL_GetTicks() < end) {
        SDL_UpdateGamepads();
        log_buttons(pad, &seen);
        const Sint16 left_x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        left_x_min = SDL_min(left_x_min, left_x);
        left_x_max = SDL_max(left_x_max, left_x);
        SDL_Delay(10);
    }
    xsdk_log_write("sdl: left stick x ran %d to %d; left trigger reads %d", left_x_min, left_x_max,
                   SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    check("sdl: the gamepad is still connected after reading it", SDL_GamepadConnected(pad));

    xsdk_log_write("sdl: the heavy motor rumbles for %d ms, then the light one", RUMBLE_MS);
    const bool heavy = SDL_RumbleGamepad(pad, 0xFFFF, 0, RUMBLE_MS);
    SDL_Delay(RUMBLE_MS + 100);
    const bool light = SDL_RumbleGamepad(pad, 0, 0xFFFF, RUMBLE_MS);
    SDL_Delay(RUMBLE_MS + 100);
    check("sdl: the gamepad rumbles", heavy && light);
    SDL_CloseGamepad(pad);
}

typedef struct
{
    float phase;
    Uint64 frames;
} Tone;

static void SDLCALL tone_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
    (void)total_amount;
    Tone *tone = (Tone *)userdata;
    float samples[512];
    int frames = additional_amount / (int)(2 * sizeof(float));
    while (frames > 0) {
        const int now = SDL_min(frames, (int)SDL_arraysize(samples) / 2);
        for (int i = 0; i < now; i++) {
            const float value = TONE_AMPLITUDE * SDL_sinf(tone->phase);
            samples[2 * i] = samples[2 * i + 1] = value;
            tone->phase += 2.0f * SDL_PI_F * TONE_HZ / TONE_RATE;
            if (tone->phase > 2.0f * SDL_PI_F) {
                tone->phase -= 2.0f * SDL_PI_F;
            }
        }
        SDL_PutAudioStreamData(stream, samples, now * 2 * (int)sizeof(float));
        tone->frames += (Uint64)now;
        frames -= now;
    }
}

static void check_audio(void)
{
    const char *driver = SDL_GetCurrentAudioDriver();
    xsdk_log_write("sdl: audio driver %s", driver ? driver : "(none)");
    check("sdl: the audio driver is xsdk's", driver && SDL_strcmp(driver, "xsdk") == 0);

    const SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, TONE_RATE };
    Tone tone = { 0.0f, 0 };
    SDL_AudioStream *stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, tone_callback, &tone);
    check("sdl: an audio stream opens on the default device", stream != NULL);
    if (!stream) {
        return;
    }
    SDL_AudioSpec device_spec;
    int device_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device_spec, &device_frames)) {
        xsdk_log_write("sdl: the device runs %d channels of format 0x%04X at %d Hz, %d frames a buffer",
                       device_spec.channels, (unsigned)device_spec.format, device_spec.freq, device_frames);
    }
    xsdk_log_write("sdl: playing a %d Hz tone for %d ms", TONE_HZ, TONE_MS);
    SDL_ResumeAudioStreamDevice(stream);
    SDL_Delay(TONE_MS);
    XsdkAudioStats stats;
    xsdk_audio_stats(&stats);
    SDL_DestroyAudioStream(stream);
    xsdk_log_write("sdl: xsdk handed the kernel %u frames and %u silent ones, %u queued at the fewest, a freed slot "
                   "filled %u us after at most",
                   (unsigned)stats.played, (unsigned)stats.silent, (unsigned)stats.fewest_queued,
                   (unsigned)stats.longest_wait_us);
    check("sdl: the kernel got no silent frame from the device's opening to the tone's end",
          stats.played > 0 && stats.silent == 0);
    const Uint64 expected = (Uint64)TONE_RATE * TONE_MS / 1000;
    xsdk_log_write("sdl: the stream asked for %u frames of the tone, %u expected", (unsigned)tone.frames,
                   (unsigned)expected);
    check("sdl: SDL pulled the tone at 48 kHz, within a tenth", tone.frames >= expected * 9 / 10 &&
                                                                     tone.frames <= expected * 11 / 10);
}

int main(void)
{
    xsdk_log_write("SDL check title");
    check("splash: the vulkan4xenon logo shown", NT_SUCCESS(xsdk_splash_show(SPLASH_MS)));
    check("sdl: SDL_Init with audio, gamepads and events", SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS));
    xsdk_log_write("sdl: platform %s, SDL %d.%d.%d", SDL_GetPlatform(), SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                   SDL_MICRO_VERSION);
    check("sdl: the platform is the Xbox 360", SDL_strcmp(SDL_GetPlatform(), "Xbox 360") == 0);
    check_paths();
    check_timer();
    check_thread();
    check_gamepad();
    check_audio();
    SDL_Quit();
    xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
    return failed_count;
}
