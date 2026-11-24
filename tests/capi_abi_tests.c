#include "avopsemu_capi.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(uint32_t) == 4, "uint32_t width");
_Static_assert(sizeof(uint64_t) == 8, "uint64_t width");
_Static_assert(sizeof(AvpsAbiInfo) == 24, "AvpsAbiInfo x64 layout");
_Static_assert(sizeof(AvpsCreateInfo) == 16, "AvpsCreateInfo layout");
_Static_assert(offsetof(AvpsErrorBuffer, utf8_buffer) == 8,
               "AvpsErrorBuffer pointer offset");
_Static_assert(offsetof(AvpsErrorBuffer, capacity_bytes) == 16,
               "AvpsErrorBuffer capacity offset");
_Static_assert(offsetof(AvpsErrorBuffer, required_bytes) == 24,
               "AvpsErrorBuffer required size offset");
_Static_assert(sizeof(AvpsErrorBuffer) == 32, "AvpsErrorBuffer x64 layout");
_Static_assert(sizeof(AvpsSessionInfo) == 32, "AvpsSessionInfo layout");

static int check(int condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        return 0;
    }
    return 1;
}

int main(void) {
    AvpsAbiInfo abi;
    memset(&abi, 0, sizeof(abi));
    abi.struct_size = (uint32_t)sizeof(abi);
    if (!check(avps_get_abi_info(&abi) == AVPS_STATUS_OK,
               "query ABI info")) {
        return 1;
    }
    if (!check(abi.abi_version == AVOPSEMU_CAPI_ABI_VERSION &&
                   abi.minimum_abi_version == AVOPSEMU_CAPI_ABI_VERSION &&
                   abi.current_abi_version == AVOPSEMU_CAPI_ABI_VERSION &&
                   (abi.feature_bits & AVPS_FEATURE_CORE_LIFECYCLE) != 0 &&
                   (abi.feature_bits & AVPS_FEATURE_DATA_SNAPSHOT) != 0 &&
                   (abi.feature_bits & AVPS_FEATURE_INPUT_AUDIO_STATUS) != 0,
               "ABI version fields")) {
        return 1;
    }

    AvpsCreateInfo create_info = {
        (uint32_t)sizeof(AvpsCreateInfo),
        AVOPSEMU_CAPI_ABI_VERSION,
        0,
        0
    };
    char error_text[8];
    AvpsErrorBuffer error = {
        (uint32_t)sizeof(AvpsErrorBuffer),
        AVOPSEMU_CAPI_ABI_VERSION,
        error_text,
        sizeof(error_text),
        0
    };

    AvpsSessionHandle previous_handle = 0;
    for (unsigned int i = 0; i < 32; ++i) {
        AvpsSessionHandle handle = 0;
        if (!check(avps_session_create(&create_info, &handle, &error) ==
                       AVPS_STATUS_OK,
                   "create session")) {
            return 1;
        }
        if (!check(handle != 0, "created handle is nonzero")) {
            return 1;
        }
        if (!check(handle > previous_handle,
                   "session handles are monotonic and never reused")) {
            return 1;
        }
        previous_handle = handle;

        AvpsSessionHandle second_handle = UINT64_C(0xfeed);
        if (!check(avps_session_create(&create_info, &second_handle, &error) ==
                       AVPS_STATUS_SESSION_LIMIT &&
                       second_handle == 0,
                   "reject a second concurrent live session")) {
            return 1;
        }

        AvpsSessionInfo state;
        memset(&state, 0, sizeof(state));
        state.struct_size = (uint32_t)sizeof(state);
        state.abi_version = AVOPSEMU_CAPI_ABI_VERSION;
        if (!check(avps_session_get_state(handle, &state, &error) ==
                       AVPS_STATUS_OK,
                   "query created state")) {
            return 1;
        }
        if (!check(state.run_state == AVPS_STATE_CREATED &&
                       state.flags == 0 && state.instructions == 0,
                   "initial session state")) {
            return 1;
        }
        if (!check(avps_session_destroy(handle, &error) == AVPS_STATUS_OK,
                   "destroy session")) {
            return 1;
        }
        memset(&state, 0, sizeof(state));
        state.struct_size = (uint32_t)sizeof(state);
        state.abi_version = AVOPSEMU_CAPI_ABI_VERSION;
        if (!check(avps_session_get_state(handle, &state, &error) ==
                       AVPS_STATUS_INVALID_HANDLE,
                   "query rejects stale handle")) {
            return 1;
        }
        if (!check(avps_session_destroy(handle, &error) ==
                       AVPS_STATUS_INVALID_HANDLE,
                   "destroy rejects stale handle")) {
            return 1;
        }
    }

    if (!check(avps_session_destroy(0, &error) == AVPS_STATUS_INVALID_HANDLE,
               "zero handle is invalid")) {
        return 1;
    }

    AvpsCreateInfo incompatible = create_info;
    incompatible.abi_version = UINT32_C(0x00020000);
    AvpsSessionHandle rejected_handle = UINT64_C(0xfeed);
    char short_error[2] = {'x', 'x'};
    error.utf8_buffer = short_error;
    error.capacity_bytes = sizeof(short_error);
    error.required_bytes = 0;
    const AvpsStatus mismatch =
        avps_session_create(&incompatible, &rejected_handle, &error);
    if (!check(mismatch == AVPS_STATUS_ABI_MISMATCH,
               "reject incompatible ABI major")) {
        return 1;
    }
    if (!check(rejected_handle == 0, "failed create clears output handle")) {
        return 1;
    }
    if (!check(error.required_bytes > sizeof(short_error) &&
                   short_error[sizeof(short_error) - 1] == '\0',
               "short error buffer is NUL terminated and reports required size")) {
        return 1;
    }

    AvpsSessionInfo too_small;
    memset(&too_small, 0, sizeof(too_small));
    too_small.struct_size = 4;
    too_small.abi_version = AVOPSEMU_CAPI_ABI_VERSION;
    if (!check(avps_session_get_state(1, &too_small, &error) ==
                   AVPS_STATUS_STRUCT_TOO_SMALL,
               "reject undersized output struct")) {
        return 1;
    }

    puts("PASS: C header layout, ABI negotiation, create/state/destroy, stale handles and error buffer rules");
    return 0;
}
