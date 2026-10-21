/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_XSDK

/* The Xbox 360's audio through xsdk. The kernel's render driver calls back for a frame every 256 samples at 48 kHz,
   six channels of big-endian floats one after another, in 5.1 order (xsdk:docs/HARDWARE.md, "Audio"). SDL's device
   thread fills a ring of such frames, and the callback hands the kernel the oldest, or silence if SDL is behind.

   A callback can still be running when unregistering returns: Xenia calls it outside the lock that unregistering
   takes (xenia:src/xenia/apu/audio_system.cc:120-130). So what callbacks touch is static and never freed; SDL opens
   the one device once at a time. */

#include "xsdk/kernel.h"

#include "../SDL_sysaudio.h"

#define XSDKAUD_RATE     48000
#define XSDKAUD_CHANNELS 6
#define XSDKAUD_SAMPLES  256
#define XSDKAUD_FLOATS   (XSDKAUD_CHANNELS * XSDKAUD_SAMPLES)
// Frames queued ahead of the kernel, about 21 ms; the one it last took stays untouched until it asks again.
#define XSDKAUD_RING 4

struct SDL_PrivateAudioData
{
    // XSDKAUD_RING frames, then a silent one, in physical memory as the kernel's driver reads them.
    float *frames;
    // SDL mixes interleaved here, and PlayDevice lays it out channel after channel in the ring.
    float *mix;
    SDL_AtomicU32 driver;
    SDL_AtomicInt written;
    SDL_AtomicInt played;
};

static struct SDL_PrivateAudioData XSDKAUD_hidden;

// Runs on the kernel's audio thread each time the driver wants a frame.
static void XSDKAUD_KernelCallback(const Uint32 *context)
{
    struct SDL_PrivateAudioData *hidden = (struct SDL_PrivateAudioData *)(uintptr_t)*context;
    const Uint32 driver = SDL_GetAtomicU32(&hidden->driver);
    // The first callbacks can come before registration has returned the driver.
    if (!driver) {
        return;
    }
    const int played = SDL_GetAtomicInt(&hidden->played);
    const float *frame = hidden->frames + XSDKAUD_RING * XSDKAUD_FLOATS;
    if (SDL_GetAtomicInt(&hidden->written) != played) {
        frame = hidden->frames + (played % XSDKAUD_RING) * XSDKAUD_FLOATS;
        SDL_SetAtomicInt(&hidden->played, played + 1);
    }
    (void)XAudioSubmitRenderDriverFrame(driver, frame);
}

static bool XSDKAUD_OpenDevice(SDL_AudioDevice *device)
{
    if (device->recording) {
        return SDL_SetError("xsdk audio has no recording");
    }
    struct SDL_PrivateAudioData *hidden = &XSDKAUD_hidden;
    device->hidden = hidden;
    SDL_SetAtomicInt(&hidden->written, 0);
    SDL_SetAtomicInt(&hidden->played, 0);
    // The driver's one format; SDL converts whatever it's given.
    device->spec.format = SDL_AUDIO_F32BE;
    device->spec.channels = XSDKAUD_CHANNELS;
    device->spec.freq = XSDKAUD_RATE;
    device->sample_frames = XSDKAUD_SAMPLES;
    SDL_UpdatedAudioDeviceFormat(device);

    hidden->mix = (float *)SDL_malloc(device->buffer_size);
    const Uint32 frames_bytes = (XSDKAUD_RING + 1) * XSDKAUD_FLOATS * sizeof(float);
    if (!hidden->frames) {
        hidden->frames = (float *)MmAllocatePhysicalMemoryEx(0, frames_bytes, PAGE_READWRITE, 0, UINT32_MAX, 0);
    }
    if (!hidden->mix || !hidden->frames) {
        return SDL_SetError("Couldn't allocate audio frames");
    }
    SDL_memset(hidden->frames, 0, frames_bytes);

    const XAUDIO_RENDER_DRIVER_CLIENT client = { XSDKAUD_KernelCallback, (Uint32)(uintptr_t)hidden };
    Uint32 driver = 0;
    const Uint32 result = XAudioRegisterRenderDriverClient(&client, &driver);
    if (result != ERROR_SUCCESS) {
        return SDL_SetError("XAudioRegisterRenderDriverClient failed: 0x%08X", (unsigned)result);
    }
    SDL_SetAtomicU32(&hidden->driver, driver);
    return true;
}

static bool XSDKAUD_WaitDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *hidden = device->hidden;
    while (!SDL_GetAtomicInt(&device->shutdown) &&
           SDL_GetAtomicInt(&hidden->written) - SDL_GetAtomicInt(&hidden->played) >= XSDKAUD_RING - 1) {
        SDL_Delay(1);
    }
    return true;
}

static Uint8 *XSDKAUD_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    return (Uint8 *)device->hidden->mix;
}

static bool XSDKAUD_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buffer_size)
{
    struct SDL_PrivateAudioData *hidden = device->hidden;
    const int written = SDL_GetAtomicInt(&hidden->written);
    float *frame = hidden->frames + (written % XSDKAUD_RING) * XSDKAUD_FLOATS;
    const float *mix = (const float *)buffer;
    for (int sample = 0; sample < XSDKAUD_SAMPLES; sample++) {
        for (int channel = 0; channel < XSDKAUD_CHANNELS; channel++) {
            frame[channel * XSDKAUD_SAMPLES + sample] = mix[sample * XSDKAUD_CHANNELS + channel];
        }
    }
    SDL_SetAtomicInt(&hidden->written, written + 1);
    return true;
}

static void XSDKAUD_CloseDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *hidden = device->hidden;
    if (!hidden) {
        return;
    }
    const Uint32 driver = SDL_GetAtomicU32(&hidden->driver);
    if (driver) {
        SDL_SetAtomicU32(&hidden->driver, 0);
        (void)XAudioUnregisterRenderDriverClient(driver);
    }
    // Only SDL's device thread uses the mix, and it has stopped.
    SDL_free(hidden->mix);
    hidden->mix = NULL;
    device->hidden = NULL;
}

// SDL's default asks for TIME_CRITICAL, which reaches the kernel as 16 since xsdk's milestone 55, and the thread then
// runs ahead of the title's own each time WaitDevice polls: Dusklight's title scene lost 2.5% in its heaviest windows.
// At NORMAL it ran as fast as before (xsdk:results/2026-10-21-milestone-55-acceptance/README.md).
static void XSDKAUD_ThreadInit(SDL_AudioDevice *device)
{
    (void)device;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
}

static bool XSDKAUD_Init(SDL_AudioDriverImpl *impl)
{
    impl->ThreadInit = XSDKAUD_ThreadInit;
    impl->OpenDevice = XSDKAUD_OpenDevice;
    impl->WaitDevice = XSDKAUD_WaitDevice;
    impl->GetDeviceBuf = XSDKAUD_GetDeviceBuf;
    impl->PlayDevice = XSDKAUD_PlayDevice;
    impl->CloseDevice = XSDKAUD_CloseDevice;
    impl->OnlyHasDefaultPlaybackDevice = true;
    return true;
}

AudioBootStrap XSDKAUD_bootstrap = {
    "xsdk", "Xbox 360 audio through xsdk", XSDKAUD_Init, false, false
};

#endif // SDL_AUDIO_DRIVER_XSDK
