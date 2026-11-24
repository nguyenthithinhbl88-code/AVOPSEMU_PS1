#include "avopsemu_capi.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

_Static_assert(sizeof(AvpsInputStatus) == 16, "AvpsInputStatus layout");
_Static_assert(offsetof(AvpsAudioStatus, underruns) == 40,
               "AvpsAudioStatus underruns offset");
_Static_assert(offsetof(AvpsAudioStatus, dropped_frames) == 48,
               "AvpsAudioStatus dropped frames offset");
_Static_assert(sizeof(AvpsAudioStatus) == 56, "AvpsAudioStatus x64 layout");

static int check(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 0;
    }
    return 1;
}

static AvpsStatus pad_status_of(AvpsSessionHandle handle,
                                AvpsInputStatus* status,
                                AvpsErrorBuffer* error) {
    memset(status, 0, sizeof(*status));
    status->struct_size = (uint32_t)sizeof(*status);
    status->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
    return avps_session_get_pad_status(handle, status, error);
}

static AvpsStatus audio_status_of(AvpsSessionHandle handle,
                                  AvpsAudioStatus* status,
                                  AvpsErrorBuffer* error) {
    memset(status, 0, sizeof(*status));
    status->struct_size = (uint32_t)sizeof(*status);
    status->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
    return avps_session_get_audio_status(handle, status, error);
}

static AvpsStatus session_status_of(AvpsSessionHandle handle,
                                    AvpsSessionInfo* status,
                                    AvpsErrorBuffer* error) {
    memset(status, 0, sizeof(*status));
    status->struct_size = (uint32_t)sizeof(*status);
    status->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
    return avps_session_get_state(handle, status, error);
}

static AvpsStatus create_session(uint32_t flags,
                                 AvpsSessionHandle* handle,
                                 AvpsErrorBuffer* error) {
    AvpsCreateInfo info = {
        (uint32_t)sizeof(AvpsCreateInfo),
        AVOPSEMU_CAPI_ABI_VERSION,
        flags,
        0
    };
    return avps_session_create(&info, handle, error);
}

static int start_succeeded(AvpsStatus status) {
    return status == AVPS_STATUS_OK ||
           status == AVPS_STATUS_WARNING_AUDIO_UNAVAILABLE;
}

int main(int argc, char** argv) {
    if (!check(argc == 2, "expected homebrew ISO fixture path")) {
        return 1;
    }

    char error_text[256];
    AvpsErrorBuffer error = {
        (uint32_t)sizeof(AvpsErrorBuffer),
        AVOPSEMU_CAPI_ABI_VERSION,
        error_text,
        sizeof(error_text),
        0
    };

    AvpsSessionHandle handle = 0;
    if (!check(create_session(0, &handle, &error) == AVPS_STATUS_OK,
               "create normal audio session")) {
        return 1;
    }

    AvpsInputStatus pad;
    if (!check(pad_status_of(handle, &pad, &error) == AVPS_STATUS_OK &&
                   pad.buttons_active_low == UINT32_C(0xffff) &&
                   pad.pending_presses == 0,
               "new pad starts neutral")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(avps_session_set_pad_buttons(handle, UINT16_C(0xfffe),
                                            &error) == AVPS_STATUS_OK &&
                   pad_status_of(handle, &pad, &error) == AVPS_STATUS_OK &&
                   pad.buttons_active_low == UINT32_C(0xfffe) &&
                   pad.pending_presses == 1,
               "active-low button press and edge are published")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(avps_session_set_pad_buttons(handle, UINT16_C(0xffff),
                                            &error) == AVPS_STATUS_OK &&
                   pad_status_of(handle, &pad, &error) == AVPS_STATUS_OK &&
                   pad.buttons_active_low == UINT32_C(0xffff) &&
                   pad.pending_presses == 1,
               "normal release preserves an unconsumed SIO edge")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(avps_session_set_pad_buttons(handle, UINT16_C(0xfffd),
                                            &error) == AVPS_STATUS_OK &&
                   avps_session_release_pad(handle, &error) == AVPS_STATUS_OK &&
                   pad_status_of(handle, &pad, &error) == AVPS_STATUS_OK &&
                   pad.buttons_active_low == UINT32_C(0xffff) &&
                   pad.pending_presses == 0,
               "focus-loss release clears held input and pending edges")) {
        avps_session_destroy(handle, &error);
        return 1;
    }

    if (!check(avps_session_set_audio_muted(handle, 2, &error) ==
                   AVPS_STATUS_INVALID_ARGUMENT,
               "reject non-boolean mute state")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(avps_session_set_audio_muted(handle, 1, &error) ==
                   AVPS_STATUS_OK,
               "mute output before opening device")) {
        avps_session_destroy(handle, &error);
        return 1;
    }

    AvpsAudioStatus audio;
    if (!check(audio_status_of(handle, &audio, &error) == AVPS_STATUS_OK &&
                   (audio.flags & AVPS_AUDIO_FLAG_MUTED) != 0 &&
                   audio.sample_rate_hz == 44100 &&
                   audio.channels == 2 && audio.bits_per_sample == 16,
               "audio status reports muted PCM format")) {
        avps_session_destroy(handle, &error);
        return 1;
    }

    if (!check(avps_session_mount_disc(handle, argv[1], &error) ==
                   AVPS_STATUS_OK &&
                   avps_session_boot_disc(handle, AVPS_BOOT_ROUTE_DIRECT_EXE,
                                          &error) == AVPS_STATUS_OK,
               "boot the CPU test program before audio checks")) {
        avps_session_destroy(handle, &error);
        return 1;
    }

    const AvpsStatus normal_start = avps_session_start(handle, &error);
    if (!check(start_succeeded(normal_start),
               "normal start does not depend on an audio device")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    Sleep(20);
    AvpsSessionInfo state;
    if (!check(session_status_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_RUNNING &&
                   state.instructions > 0,
               "CPU progresses with normal audio result")) {
        avps_session_stop(handle, &error);
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(audio_status_of(handle, &audio, &error) == AVPS_STATUS_OK &&
                   ((audio.flags & AVPS_AUDIO_FLAG_AVAILABLE) != 0 ||
                    normal_start == AVPS_STATUS_WARNING_AUDIO_UNAVAILABLE),
               "audio status reports the real device-open result")) {
        avps_session_stop(handle, &error);
        avps_session_destroy(handle, &error);
        return 1;
    }
    printf("INFO: physical audio output %s; native MMRESULT=0x%08x\n",
           (audio.flags & AVPS_AUDIO_FLAG_AVAILABLE) ? "available" : "unavailable",
           audio.native_error);

    if (!check(avps_session_set_audio_muted(handle, 0, &error) ==
                   AVPS_STATUS_OK &&
                   audio_status_of(handle, &audio, &error) == AVPS_STATUS_OK &&
                   (audio.flags & AVPS_AUDIO_FLAG_MUTED) == 0,
               "unmute is reflected immediately in status")) {
        avps_session_stop(handle, &error);
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(avps_session_stop(handle, &error) == AVPS_STATUS_OK,
               "stop normal audio session")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(avps_session_destroy(handle, &error) == AVPS_STATUS_OK,
               "destroy normal audio session")) {
        return 1;
    }

    handle = 0;
    if (!check(create_session(AVPS_CREATE_FLAG_DISABLE_AUDIO, &handle, &error) ==
                   AVPS_STATUS_OK,
               "create headless audio-disabled session")) {
        return 1;
    }
    if (!check(avps_session_mount_disc(handle, argv[1], &error) ==
                   AVPS_STATUS_OK &&
                   avps_session_boot_disc(handle, AVPS_BOOT_ROUTE_DIRECT_EXE,
                                          &error) == AVPS_STATUS_OK,
               "boot the CPU test program without audio output")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(avps_session_set_audio_muted(handle, 1, &error) ==
                   AVPS_STATUS_OK,
               "mute headless session")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    const AvpsStatus headless_start = avps_session_start(handle, &error);
    if (!check(headless_start == AVPS_STATUS_WARNING_AUDIO_UNAVAILABLE,
               "audio-disabled start returns nonfatal warning")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    Sleep(20);
    if (!check(session_status_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_RUNNING &&
                   state.instructions > 0,
               "CPU continues running with no audio output")) {
        avps_session_stop(handle, &error);
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(audio_status_of(handle, &audio, &error) == AVPS_STATUS_OK &&
                   (audio.flags & AVPS_AUDIO_FLAG_AVAILABLE) == 0 &&
                   (audio.flags & AVPS_AUDIO_FLAG_DISABLED_BY_REQUEST) != 0 &&
                   (audio.flags & AVPS_AUDIO_FLAG_MUTED) != 0,
               "headless audio status distinguishes disabled output")) {
        avps_session_stop(handle, &error);
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(avps_session_stop(handle, &error) == AVPS_STATUS_OK &&
                   avps_session_destroy(handle, &error) == AVPS_STATUS_OK,
               "stop and destroy headless session")) {
        return 1;
    }

    puts("PASS: active-low HostPad input, focus-loss release, mute/status and CPU progress with audio unavailable");
    return 0;
}
