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

/* The Xbox 360's audio through xsdk's audio API, which feeds the kernel's render driver frames of 256 samples at 48 kHz,
   six channels of big-endian floats one after another, in 5.1 order (xsdk:include/xsdk/audio.h). SDL's device thread
   is the feeder: WaitDevice blocks until the kernel has taken a frame, and PlayDevice lays SDL's mix out in the next. */

#include "xsdk/audio.h"

#include "../SDL_sysaudio.h"

// WaitDevice's longest wait before it looks for a shutdown again; the kernel frees a slot every 5.33 ms.
#define XSDKAUD_WAIT_MS 100

struct SDL_PrivateAudioData
{
    // SDL mixes interleaved here, and PlayDevice lays it out channel after channel in xsdk's frame.
    float *mix;
};

static struct SDL_PrivateAudioData XSDKAUD_hidden;

/* The one device, in the driver's format. SDL keeps a driver's spec as the device's (SDL_audio.c:664-665, 716), gives it
   to SDL_GetAudioDeviceFormat before the device opens (1620), and opens with it when the title asks for none (1811), so
   SDL, and FAudio's mastering voice by default, mix at 48 kHz in 5.1 without converting. */
static void XSDKAUD_DetectDevices(SDL_AudioDevice **default_playback, SDL_AudioDevice **default_recording)
{
    (void)default_recording;
    const SDL_AudioSpec spec = { SDL_AUDIO_F32BE, XSDK_AUDIO_CHANNELS, XSDK_AUDIO_RATE_HZ };
    *default_playback = SDL_AddAudioDevice(false, DEFAULT_PLAYBACK_DEVNAME, &spec, (void *)((size_t)0x1));
}

static bool XSDKAUD_OpenDevice(SDL_AudioDevice *device)
{
    if (device->recording) {
        return SDL_SetError("xsdk audio has no recording");
    }
    struct SDL_PrivateAudioData *hidden = &XSDKAUD_hidden;
    device->hidden = hidden;
    // The driver's one format; SDL converts whatever it's given.
    device->spec.format = SDL_AUDIO_F32BE;
    device->spec.channels = XSDK_AUDIO_CHANNELS;
    device->spec.freq = XSDK_AUDIO_RATE_HZ;
    device->sample_frames = XSDK_AUDIO_FRAME_SAMPLES;
    SDL_UpdatedAudioDeviceFormat(device);

    hidden->mix = (float *)SDL_malloc(device->buffer_size);
    if (!hidden->mix) {
        return false;
    }
    const NTSTATUS status = xsdk_audio_open(0);
    if (!NT_SUCCESS(status)) {
        return SDL_SetError("xsdk_audio_open failed: 0x%08X", (unsigned)status);
    }
    return true;
}

// Blocks until the kernel has taken a frame and a slot is free, or SDL shuts the device down.
static bool XSDKAUD_WaitDevice(SDL_AudioDevice *device)
{
    while (!SDL_GetAtomicInt(&device->shutdown)) {
        float *frame;
        const NTSTATUS status = xsdk_audio_next_frame(XSDKAUD_WAIT_MS, &frame);
        if (status != STATUS_IO_TIMEOUT) {
            return NT_SUCCESS(status);
        }
    }
    return true;
}

static Uint8 *XSDKAUD_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    return (Uint8 *)device->hidden->mix;
}

static bool XSDKAUD_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buffer_size)
{
    // WaitDevice found a slot free, as the empty ring had for the first mix.
    float *frame;
    const NTSTATUS status = xsdk_audio_next_frame(0, &frame);
    if (!NT_SUCCESS(status)) {
        return status == STATUS_IO_TIMEOUT;
    }
    volatile float *out = frame;
    const float *mix = (const float *)buffer;
    for (int sample = 0; sample < XSDK_AUDIO_FRAME_SAMPLES; sample++) {
        for (int channel = 0; channel < XSDK_AUDIO_CHANNELS; channel++) {
            out[channel * XSDK_AUDIO_FRAME_SAMPLES + sample] = mix[sample * XSDK_AUDIO_CHANNELS + channel];
        }
    }
    xsdk_audio_queue_frame();
    return true;
}

static void XSDKAUD_CloseDevice(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *hidden = device->hidden;
    if (!hidden) {
        return;
    }
    // Fails harmlessly when OpenDevice didn't get as far as opening it.
    (void)xsdk_audio_close();
    // Only SDL's device thread uses the mix, and it has stopped.
    SDL_free(hidden->mix);
    hidden->mix = NULL;
    device->hidden = NULL;
}

// SDL's default asks for TIME_CRITICAL, 16 on the console. Left at its default, the thread is raised to +1 by
// xsdk_audio_next_frame, where a feeder beside a busy thread woke within 3 us of each frame the kernel took
// (xsdk:docs/HARDWARE.md, "Audio's feeding, milestone 58").
static void XSDKAUD_ThreadInit(SDL_AudioDevice *device)
{
    (void)device;
}

static bool XSDKAUD_Init(SDL_AudioDriverImpl *impl)
{
    impl->DetectDevices = XSDKAUD_DetectDevices;
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
