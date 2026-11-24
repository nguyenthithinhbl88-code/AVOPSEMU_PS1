#include "avopsemu_capi.h"

#include <stdio.h>
#include <windows.h>

typedef AvpsStatus(AVOPSEMU_CAPI_CALL* AvpsGetAbiInfoFn)(AvpsAbiInfo*);

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Expected deployed DLL path\n");
        return 1;
    }

    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    AvpsGetAbiInfoFn get_abi_info = (AvpsGetAbiInfoFn)GetProcAddress(
        module, "avps_get_abi_info");
    if (!get_abi_info) {
        fprintf(stderr, "Deployed DLL does not export avps_get_abi_info\n");
        FreeLibrary(module);
        return 1;
    }

    AvpsAbiInfo info = {0};
    info.struct_size = (uint32_t)sizeof(info);
    const AvpsStatus status = get_abi_info(&info);
    const uint64_t required_features =
        AVPS_FEATURE_CORE_LIFECYCLE | AVPS_FEATURE_DATA_SNAPSHOT |
        AVPS_FEATURE_INPUT_AUDIO_STATUS;
    if (status != AVPS_STATUS_OK ||
        info.abi_version != AVOPSEMU_CAPI_ABI_VERSION ||
        (info.feature_bits & required_features) != required_features) {
        fprintf(stderr, "Deployed DLL ABI probe failed: status=0x%08x version=0x%08x features=0x%016llx\n",
                (unsigned)status, (unsigned)info.abi_version,
                (unsigned long long)info.feature_bits);
        FreeLibrary(module);
        return 1;
    }

    FreeLibrary(module);
    puts("PASS: LoadLibrary opened the DLL copied into the WPF output folder and the ABI entry point responded");
    return 0;
}
