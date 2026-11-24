#include "avopsemu_capi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

_Static_assert(offsetof(AvpsCpuSnapshot, instructions) == 280,
               "AvpsCpuSnapshot instruction offset");
_Static_assert(sizeof(AvpsCpuSnapshot) == 328,
               "AvpsCpuSnapshot x64 layout");
_Static_assert(offsetof(AvpsVramInfo, required_bytes) == 24,
               "AvpsVramInfo size offset");
_Static_assert(sizeof(AvpsVramInfo) == 48, "AvpsVramInfo x64 layout");
_Static_assert(offsetof(AvpsVideoInfo, required_bytes) == 48,
               "AvpsVideoInfo required size offset");
_Static_assert(sizeof(AvpsVideoInfo) == 72, "AvpsVideoInfo x64 layout");

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

static int copy_display_with_retry(AvpsSessionHandle handle,
                                   AvpsVideoInfo* info,
                                   uint8_t** pixels,
                                   AvpsErrorBuffer* error) {
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        memset(info, 0, sizeof(*info));
        info->struct_size = (uint32_t)sizeof(*info);
        info->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
        const AvpsStatus query = avps_session_copy_display_rgba8(
            handle, info, NULL, 0, error);
        if (!check(query == AVPS_STATUS_BUFFER_TOO_SMALL,
                   "display size query reports required bytes")) {
            return 0;
        }
        uint8_t* output = (uint8_t*)malloc((size_t)info->required_bytes);
        if (!check(output != NULL, "allocate display snapshot buffer")) {
            return 0;
        }
        const AvpsStatus copied = avps_session_copy_display_rgba8(
            handle, info, output, info->required_bytes, error);
        if (copied == AVPS_STATUS_OK) {
            *pixels = output;
            return 1;
        }
        free(output);
        if (copied != AVPS_STATUS_BUFFER_TOO_SMALL) {
            return check(0, "copy display snapshot");
        }
    }
    return check(0, "display snapshot size remained stable enough to copy");
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
    AvpsCreateInfo create_info = {
        (uint32_t)sizeof(AvpsCreateInfo),
        AVOPSEMU_CAPI_ABI_VERSION,
        0,
        0
    };
    AvpsSessionHandle handle = 0;
    if (!check(avps_session_create(&create_info, &handle, &error) ==
                   AVPS_STATUS_OK,
               "create snapshot session")) {
        return 1;
    }
    if (!check(avps_session_mount_disc(handle, argv[1], &error) ==
                   AVPS_STATUS_OK &&
                   avps_session_boot_disc(handle, AVPS_BOOT_ROUTE_DIRECT_EXE,
                                          &error) == AVPS_STATUS_OK,
               "boot homebrew ISO directly")) {
        avps_session_destroy(handle, &error);
        return 1;
    }

    AvpsStatus start_status = avps_session_start(handle, &error);
    if (!check(start_status == AVPS_STATUS_OK ||
                   start_status == AVPS_STATUS_WARNING_AUDIO_UNAVAILABLE,
               "start Runner for live CPU snapshot")) {
        avps_session_destroy(handle, &error);
        return 1;
    }

    AvpsCpuSnapshot cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.struct_size = (uint32_t)sizeof(cpu);
    cpu.abi_version = AVOPSEMU_CAPI_ABI_VERSION;
    int got_live_snapshot = 0;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const AvpsStatus snapshot_status =
            avps_session_get_cpu_snapshot(handle, &cpu, &error);
        if (snapshot_status != AVPS_STATUS_OK) {
            avps_session_stop(handle, &error);
            avps_session_destroy(handle, &error);
            return check(0, "read CPU snapshot while Runner is active");
        }
        if (cpu.instructions >= 1000) {
            got_live_snapshot = 1;
            break;
        }
        Sleep(1);
    }
    if (!check(got_live_snapshot && cpu.pc >= 0x80000000u &&
                   cpu.gpr[0] == 0,
               "live CPU snapshot contains advancing MIPS state")) {
        avps_session_stop(handle, &error);
        avps_session_destroy(handle, &error);
        return 1;
    }

    AvpsSessionInfo state;
    if (!check(state_of(handle, &state, &error) == AVPS_STATUS_OK &&
                   state.run_state == AVPS_STATE_RUNNING &&
                   state.instructions >= cpu.instructions,
               "session state agrees with live CPU snapshot")) {
        avps_session_stop(handle, &error);
        avps_session_destroy(handle, &error);
        return 1;
    }

    if (!check(avps_session_stop(handle, &error) == AVPS_STATUS_OK,
               "stop before deterministic video copies")) {
        avps_session_destroy(handle, &error);
        return 1;
    }

    AvpsVramInfo vram_info;
    memset(&vram_info, 0, sizeof(vram_info));
    vram_info.struct_size = (uint32_t)sizeof(vram_info);
    vram_info.abi_version = AVOPSEMU_CAPI_ABI_VERSION;
    if (!check(avps_session_copy_vram_rgb555(handle, &vram_info, NULL, 0,
                                              &error) ==
                   AVPS_STATUS_BUFFER_TOO_SMALL,
               "VRAM size query")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(vram_info.width == 1024 && vram_info.height == 512 &&
                   vram_info.stride_bytes == 2048 &&
                   vram_info.pixel_format == AVPS_PIXEL_FORMAT_RGB555 &&
                   vram_info.required_bytes == UINT64_C(1024) * 512 * 2,
               "VRAM metadata and exact byte count")) {
        avps_session_destroy(handle, &error);
        return 1;
    }

    uint8_t short_buffer[8];
    memset(short_buffer, 0x5a, sizeof(short_buffer));
    if (!check(avps_session_copy_vram_rgb555(handle, &vram_info, short_buffer,
                                              sizeof(short_buffer), &error) ==
                   AVPS_STATUS_BUFFER_TOO_SMALL &&
                   short_buffer[0] == 0x5a &&
                   short_buffer[sizeof(short_buffer) - 1] == 0x5a,
               "short VRAM buffer is not partially written")) {
        avps_session_destroy(handle, &error);
        return 1;
    }

    uint8_t* vram = (uint8_t*)malloc((size_t)vram_info.required_bytes);
    if (!check(vram != NULL, "allocate VRAM snapshot buffer")) {
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(avps_session_copy_vram_rgb555(
                   handle, &vram_info, vram, vram_info.required_bytes,
                   &error) == AVPS_STATUS_OK,
               "copy complete VRAM snapshot")) {
        free(vram);
        avps_session_destroy(handle, &error);
        return 1;
    }
    const size_t known_pixel = ((size_t)120 * 1024 + 160) * 2;
    if (!check(vram[known_pixel] == 0x06 && vram[known_pixel + 1] == 0x7e,
               "VRAM RGB555 pixels match executed homebrew framebuffer")) {
        free(vram);
        avps_session_destroy(handle, &error);
        return 1;
    }
    AvpsVideoInfo video_info;
    uint8_t* frame = NULL;
    if (!check(copy_display_with_retry(handle, &video_info, &frame, &error),
               "copy display frame and metadata")) {
        free(vram);
        avps_session_destroy(handle, &error);
        return 1;
    }
    if (!check(video_info.width > 0 && video_info.width <= 1024 &&
                   video_info.height > 0 && video_info.height <= 512 &&
                   video_info.stride_bytes == video_info.width * 4 &&
                   video_info.pixel_format == AVPS_PIXEL_FORMAT_RGBA8 &&
                   video_info.required_bytes ==
                       (uint64_t)video_info.stride_bytes * video_info.height &&
                   video_info.video_frames == vram_info.video_frames &&
                   video_info.display_revision == vram_info.display_revision,
               "display RGBA8 metadata and opaque pixel layout")) {
        free(frame);
        free(vram);
        avps_session_destroy(handle, &error);
        return 1;
    }

    uint8_t expected_rgb[3] = {0, 0, 0};
    if ((video_info.flags & AVPS_VIDEO_FLAG_DISABLED) == 0) {
        if ((video_info.display_mode & 16) != 0) {
            const uint64_t row = (uint64_t)(video_info.source_y & 511) * 2048;
            const unsigned byte_start = (video_info.source_x * 2) & 2047;
            for (unsigned channel = 0; channel < 3; ++channel) {
                const unsigned byte_index = (byte_start + channel) & 2047;
                expected_rgb[channel] = vram[row + byte_index];
            }
        } else {
            const size_t pixel_index =
                (size_t)(video_info.source_y & 511) * 1024 +
                (video_info.source_x & 1023);
            const uint16_t pixel =
                (uint16_t)vram[pixel_index * 2] |
                (uint16_t)((uint16_t)vram[pixel_index * 2 + 1] << 8);
            expected_rgb[0] =
                (uint8_t)(((pixel & 31) << 3) | ((pixel & 31) >> 2));
            expected_rgb[1] =
                (uint8_t)((((pixel >> 5) & 31) << 3) |
                          (((pixel >> 5) & 31) >> 2));
            expected_rgb[2] =
                (uint8_t)((((pixel >> 10) & 31) << 3) |
                          (((pixel >> 10) & 31) >> 2));
        }
    }
    if (!check(frame[0] == expected_rgb[0] &&
                   frame[1] == expected_rgb[1] &&
                   frame[2] == expected_rgb[2] && frame[3] == 0xff,
               "display RGBA pixel matches the VRAM snapshot")) {
        free(frame);
        free(vram);
        avps_session_destroy(handle, &error);
        return 1;
    }
    free(frame);
    free(vram);

    if (!check(avps_session_destroy(handle, &error) == AVPS_STATUS_OK,
               "destroy snapshot session")) {
        return 1;
    }

    puts("PASS: CPU snapshot while RUNNING, fixed-layout VRAM RGB555, display RGBA8, size negotiation and metadata/data copies");
    return 0;
}
