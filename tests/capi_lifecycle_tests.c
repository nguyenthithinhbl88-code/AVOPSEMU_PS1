#include "avopsemu_capi.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

static int check(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 0;
    }
    return 1;
}

static AvpsStatus state_of(AvpsSessionHandle handle,
                           AvpsSessionInfo* state,
                           AvpsErrorBuffer* error) {
    memset(state, 0, sizeof(*state));
    state->struct_size = (uint32_t)sizeof(*state);
    state->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
    return avps_session_get_state(handle, state, error);
}

static int is_start_success(AvpsStatus status) {
    return status == AVPS_STATUS_OK ||
           status == AVPS_STATUS_WARNING_AUDIO_UNAVAILABLE;
}

int main(int argc, char** argv) {
    if (!check(argc == 4, "expected BIOS, CUE, and non-BIOS fixture paths")) {
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
    AvpsCreateInfo create_info = {
        (uint32_t)sizeof(AvpsCreateInfo),
        AVOPSEMU_CAPI_ABI_VERSION,
        0,
        0
    };

    AvpsSessionHandle handle = 0;
    if (!check(avps_session_create(&create_info, &handle, &error) ==
                   AVPS_STATUS_OK,
               "create lifecycle session")) {
        return 1;
    }

    AvpsSessionInfo state;
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_CREATED &&
                   state.flags == 0,
               "new lifecycle session state")) {
        return 1;
    }

    if (!check(avps_session_boot_disc(handle, AVPS_BOOT_ROUTE_DIRECT_EXE,
                                      &error) == AVPS_STATUS_INVALID_STATE,
               "direct boot requires mounted media")) {
        return 1;
    }

    if (!check(avps_session_load_bios(handle, argv[3], &error) ==
                   AVPS_STATUS_FORMAT_ERROR,
               "reject BIOS with incorrect size")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   (state.flags & AVPS_SESSION_FLAG_BIOS_READY) == 0,
               "invalid BIOS does not mark BIOS ready")) {
        return 1;
    }

    if (!check(avps_session_load_bios(handle, argv[1], &error) ==
                   AVPS_STATUS_OK,
               "load valid 512-KiB BIOS")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_STOPPED &&
                   (state.flags & AVPS_SESSION_FLAG_BIOS_READY) != 0 &&
                   state.instructions == 0,
               "BIOS load resets CPU and marks BIOS ready")) {
        return 1;
    }

    if (!check(avps_session_mount_disc(handle, argv[2], &error) ==
                   AVPS_STATUS_OK,
               "mount valid CUE disc")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   (state.flags & (AVPS_SESSION_FLAG_BIOS_READY |
                                   AVPS_SESSION_FLAG_DISC_MOUNTED)) ==
                       (AVPS_SESSION_FLAG_BIOS_READY |
                        AVPS_SESSION_FLAG_DISC_MOUNTED),
               "mounted disc and BIOS flags")) {
        return 1;
    }

    char missing_disc[4096];
    if (snprintf(missing_disc, sizeof(missing_disc), "%s.missing", argv[2]) < 0 ||
        !check(avps_session_mount_disc(handle, missing_disc, &error) ==
                   AVPS_STATUS_IO_ERROR,
               "missing disc reports I/O error")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   (state.flags & AVPS_SESSION_FLAG_DISC_MOUNTED) != 0,
               "failed mount preserves mounted media")) {
        return 1;
    }

    if (!check(avps_session_boot_disc(handle, AVPS_BOOT_ROUTE_BIOS, &error) ==
                   AVPS_STATUS_OK,
               "boot mounted disc through BIOS route")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   (state.flags & AVPS_SESSION_FLAG_PROGRAM_READY) != 0 &&
                   (state.flags & AVPS_SESSION_FLAG_DIRECT_EXE_BOOT) == 0,
               "BIOS disc boot route flags")) {
        return 1;
    }

    if (!check(avps_session_boot_disc(handle, AVPS_BOOT_ROUTE_DIRECT_EXE,
                                      &error) == AVPS_STATUS_OK,
               "direct homebrew EXE boot")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   (state.flags & (AVPS_SESSION_FLAG_PROGRAM_READY |
                                   AVPS_SESSION_FLAG_DIRECT_EXE_BOOT)) ==
                       (AVPS_SESSION_FLAG_PROGRAM_READY |
                        AVPS_SESSION_FLAG_DIRECT_EXE_BOOT),
               "direct EXE route flags")) {
        return 1;
    }

    if (!check(avps_session_reset_cpu_keep_ram(handle, &error) ==
                   AVPS_STATUS_OK,
               "CPU reset with RAM retained")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_STOPPED &&
                   state.instructions == 0 &&
                   (state.flags & AVPS_SESSION_FLAG_DIRECT_EXE_BOOT) != 0,
               "CPU reset preserves loaded route state")) {
        return 1;
    }

    if (!check(avps_session_reinitialize(handle, &error) == AVPS_STATUS_OK,
               "reinitialize machine")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_STOPPED &&
                   state.instructions == 0 &&
                   (state.flags & (AVPS_SESSION_FLAG_BIOS_READY |
                                   AVPS_SESSION_FLAG_DISC_MOUNTED)) ==
                       (AVPS_SESSION_FLAG_BIOS_READY |
                        AVPS_SESSION_FLAG_DISC_MOUNTED) &&
                   (state.flags & (AVPS_SESSION_FLAG_PROGRAM_READY |
                                   AVPS_SESSION_FLAG_DIRECT_EXE_BOOT)) == 0,
               "reinitialize resets program while retaining BIOS and disc")) {
        return 1;
    }

    if (!check(avps_session_eject_disc(handle, &error) == AVPS_STATUS_OK,
               "eject mounted disc")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   (state.flags & AVPS_SESSION_FLAG_DISC_MOUNTED) == 0 &&
                   (state.flags & AVPS_SESSION_FLAG_BIOS_READY) != 0,
               "eject preserves BIOS and clears media flag")) {
        return 1;
    }
    if (!check(avps_session_boot_disc(handle, AVPS_BOOT_ROUTE_BIOS, &error) ==
                   AVPS_STATUS_INVALID_STATE,
               "BIOS disc boot requires mounted media")) {
        return 1;
    }
    if (!check(avps_session_mount_disc(handle, argv[2], &error) ==
                   AVPS_STATUS_OK &&
                   avps_session_boot_disc(handle, AVPS_BOOT_ROUTE_BIOS,
                                          &error) == AVPS_STATUS_OK,
               "remount and reboot after eject")) {
        return 1;
    }

    AvpsStatus start_status = avps_session_start(handle, &error);
    if (!check(is_start_success(start_status),
               "start CPU even when host audio is unavailable")) {
        return 1;
    }
    Sleep(20);
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_RUNNING &&
                   state.instructions > 0,
               "running state advances CPU")) {
        return 1;
    }

    if (!check(avps_session_pause(handle, &error) == AVPS_STATUS_OK,
               "pause running session")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_PAUSED,
               "paused state reported")) {
        return 1;
    }
    const uint64_t paused_instructions = state.instructions;
    Sleep(20);
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_PAUSED &&
                   state.instructions == paused_instructions,
               "paused CPU remains stopped")) {
        return 1;
    }

    start_status = avps_session_start(handle, &error);
    if (!check(is_start_success(start_status),
               "resume paused session")) {
        return 1;
    }
    Sleep(20);
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_RUNNING &&
                   state.instructions > paused_instructions,
               "resumed CPU continues execution")) {
        return 1;
    }

    if (!check(avps_session_stop(handle, &error) == AVPS_STATUS_OK,
               "stop running session")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_STOPPED,
               "stopped state reported")) {
        return 1;
    }

    start_status = avps_session_start(handle, &error);
    if (!check(is_start_success(start_status),
               "restart before destroy-while-running check")) {
        return 1;
    }
    if (!check(avps_session_destroy(handle, &error) == AVPS_STATUS_OK,
               "destroy stops and joins running workers")) {
        return 1;
    }
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_INVALID_HANDLE,
               "destroyed lifecycle handle is stale")) {
        return 1;
    }

    puts("PASS: BIOS validation/load, media mount/eject/boot routes, CPU reset/reinitialize, Runner start/pause/resume/stop and destroy while running");
    return 0;
}
