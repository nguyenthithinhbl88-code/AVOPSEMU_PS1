#include "avopsemu_capi.h"

#include "audio_win32.hpp"
#include "core.hpp"
#include "display.hpp"
#include "host_pad.hpp"
#include "runner.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <array>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t abi_major = AVOPSEMU_CAPI_ABI_MAJOR(AVOPSEMU_CAPI_ABI_VERSION);
constexpr uint32_t abi_minor = AVOPSEMU_CAPI_ABI_MINOR(AVOPSEMU_CAPI_ABI_VERSION);
constexpr uint32_t abi_minimum_version = AVOPSEMU_CAPI_ABI_VERSION;
constexpr uint32_t abi_current_version = AVOPSEMU_CAPI_ABI_VERSION;

struct NativeSession {
    // Declaration order ensures HostPad outlives Bus/SIO, Bus outlives Cpu,
    // and the workers are destroyed before their mutex and machine objects.
    ps1::HostPad host_pad;
    std::unique_ptr<ps1::Bus> bus;
    std::unique_ptr<ps1::Cpu> cpu;
    std::mutex machine_mutex;
    std::unique_ptr<ps1::Runner> runner;
    ps1::AudioOutput audio_output;
    std::mutex lifecycle_mutex;
    uint32_t run_state = AVPS_STATE_CREATED;
    uint32_t last_error_code = AVPS_STATUS_OK;
    bool bios_ready = false;
    bool disc_mounted = false;
    bool program_ready = false;
    bool direct_exe_boot = false;
    bool audio_disabled = false;
    uint32_t reset_entry = 0xbfc00000;
    std::array<uint32_t, 32> reset_registers{};
    bool restore_reset_registers = false;

    explicit NativeSession(bool disable_audio)
        : bus(std::make_unique<ps1::Bus>()),
          cpu(std::make_unique<ps1::Cpu>(*bus)),
          audio_disabled(disable_audio) {
        rebind_host_pad();
        cpu->reset();
    }

    NativeSession(const NativeSession&) = delete;
    NativeSession& operator=(const NativeSession&) = delete;

    void rebind_host_pad() noexcept {
        bus->sio[0].host_input = &host_pad;
        bus->sio[0].connected = true;
    }

    void stop_workers() {
        if (runner) {
            runner->stop();
            runner.reset();
        }
        audio_output.close();
        host_pad.clear();
    }

    void refresh_run_state() {
        if (run_state != AVPS_STATE_RUNNING || !runner || runner->active()) {
            return;
        }
        const auto failure = runner->error();
        if (failure.empty()) {
            run_state = AVPS_STATE_STOPPED;
        } else {
            run_state = AVPS_STATE_FAULTED;
            last_error_code = AVPS_STATUS_CORE_FAULT;
        }
    }

    void shutdown() noexcept {
        // Never hold machine_mutex while either worker is joined.
        try {
            if (runner) {
                runner->stop();
                runner.reset();
            }
        } catch (...) {
            // Destruction must not allow an exception across the ABI boundary.
        }
        try {
            audio_output.close();
        } catch (...) {
        }
        host_pad.clear();
    }

    ~NativeSession() noexcept {
        shutdown();
    }
};

struct RegistrySlot {
    std::shared_ptr<NativeSession> session;
    uint64_t in_flight = 0;
    bool closing = false;
    std::condition_variable drained;
};

std::mutex registry_mutex;
std::unordered_map<AvpsSessionHandle, std::shared_ptr<RegistrySlot>> registry;
uint64_t next_handle = 1;
bool session_transition = false;

struct SessionCreationReservation {
    bool active = false;

    bool acquire() {
        std::lock_guard<std::mutex> lock(registry_mutex);
        if (!registry.empty() || session_transition) {
            return false;
        }
        session_transition = true;
        active = true;
        return true;
    }

    void release() noexcept {
        if (!active) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            session_transition = false;
        }
        active = false;
    }

    ~SessionCreationReservation() {
        release();
    }
};

bool version_supported(uint32_t version) noexcept {
    return AVOPSEMU_CAPI_ABI_MAJOR(version) == abi_major &&
           AVOPSEMU_CAPI_ABI_MINOR(version) <= abi_minor;
}

AvpsStatus validate_error_buffer(AvpsErrorBuffer* error) noexcept {
    if (!error) {
        return AVPS_STATUS_OK;
    }
    constexpr uint32_t required_size =
        static_cast<uint32_t>(sizeof(AvpsErrorBuffer));
    if (error->struct_size < required_size) {
        return AVPS_STATUS_STRUCT_TOO_SMALL;
    }
    if (!version_supported(error->abi_version)) {
        return AVPS_STATUS_ABI_MISMATCH;
    }
    if (error->capacity_bytes != 0 && !error->utf8_buffer) {
        return AVPS_STATUS_INVALID_ARGUMENT;
    }
    return AVPS_STATUS_OK;
}

size_t utf8_sequence_length(const unsigned char* text, size_t remaining) noexcept {
    const unsigned char first = text[0];
    if (first < 0x80) {
        return 1;
    }
    if (first >= 0xc2 && first <= 0xdf && remaining >= 2 &&
        (text[1] & 0xc0) == 0x80) {
        return 2;
    }
    if (first >= 0xe0 && first <= 0xef && remaining >= 3 &&
        (text[1] & 0xc0) == 0x80 && (text[2] & 0xc0) == 0x80 &&
        !(first == 0xe0 && text[1] < 0xa0) &&
        !(first == 0xed && text[1] >= 0xa0)) {
        return 3;
    }
    if (first >= 0xf0 && first <= 0xf4 && remaining >= 4 &&
        (text[1] & 0xc0) == 0x80 && (text[2] & 0xc0) == 0x80 &&
        (text[3] & 0xc0) == 0x80 &&
        !(first == 0xf0 && text[1] < 0x90) &&
        !(first == 0xf4 && text[1] >= 0x90)) {
        return 4;
    }
    return 0;
}

uint64_t sanitized_utf8_size(std::string_view text) noexcept {
    uint64_t result = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        const auto* bytes =
            reinterpret_cast<const unsigned char*>(text.data() + offset);
        const size_t sequence = utf8_sequence_length(bytes, text.size() - offset);
        if (sequence == 0) {
            ++result;
            ++offset;
        } else {
            result += sequence;
            offset += sequence;
        }
    }
    return result;
}

void write_error(AvpsErrorBuffer* error, std::string_view text) noexcept {
    if (!error) {
        return;
    }

    const uint64_t text_bytes = sanitized_utf8_size(text);
    error->required_bytes = text_bytes + 1;
    if (error->capacity_bytes == 0 || !error->utf8_buffer) {
        return;
    }

    const uint64_t writable = error->capacity_bytes - 1;
    uint64_t written = 0;
    size_t offset = 0;
    while (offset < text.size()) {
        const auto* bytes =
            reinterpret_cast<const unsigned char*>(text.data() + offset);
        const size_t sequence = utf8_sequence_length(bytes, text.size() - offset);
        if (sequence == 0) {
            if (written + 1 > writable) {
                break;
            }
            error->utf8_buffer[written++] = '?';
            ++offset;
            continue;
        }
        if (written + sequence > writable) {
            break;
        }
        std::memcpy(error->utf8_buffer + written, text.data() + offset, sequence);
        written += sequence;
        offset += sequence;
    }
    error->utf8_buffer[written] = '\0';
}

void clear_error(AvpsErrorBuffer* error) noexcept {
    if (!error) {
        return;
    }
    error->required_bytes = 0;
    if (error->capacity_bytes != 0 && error->utf8_buffer) {
        error->utf8_buffer[0] = '\0';
    }
}

AvpsStatus fail(AvpsErrorBuffer* error,
                AvpsStatus status,
                const char* message) noexcept {
    write_error(error, message ? std::string_view(message) : std::string_view{});
    return status;
}

AvpsStatus fail_session(NativeSession& session,
                        AvpsErrorBuffer* error,
                        AvpsStatus status,
                        const char* message) noexcept {
    session.last_error_code = status;
    return fail(error, status, message);
}

uint32_t session_flags(const NativeSession& session) noexcept {
    uint32_t flags = 0;
    if (session.bios_ready) {
        flags |= AVPS_SESSION_FLAG_BIOS_READY;
    }
    if (session.disc_mounted) {
        flags |= AVPS_SESSION_FLAG_DISC_MOUNTED;
    }
    if (session.program_ready) {
        flags |= AVPS_SESSION_FLAG_PROGRAM_READY;
    }
    if (session.direct_exe_boot) {
        flags |= AVPS_SESSION_FLAG_DIRECT_EXE_BOOT;
    }
    return flags;
}

AvpsStatus read_bios_image(const char* path_utf8,
                           std::vector<uint8_t>& image,
                           AvpsErrorBuffer* error) {
    if (!path_utf8 || path_utf8[0] == '\0') {
        return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                    "A non-empty UTF-8 BIOS path is required");
    }
    try {
        const auto path = std::filesystem::u8path(path_utf8);
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) {
            return fail(error, AVPS_STATUS_IO_ERROR, "Could not open BIOS file");
        }
        if (input.tellg() != std::streamoff(512 * 1024)) {
            return fail(error, AVPS_STATUS_FORMAT_ERROR,
                        "BIOS must be exactly 512 KiB");
        }
        image.resize(512 * 1024);
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(image.data()),
                        static_cast<std::streamsize>(image.size()))) {
            image.clear();
            return fail(error, AVPS_STATUS_IO_ERROR, "BIOS read failed");
        }
        return AVPS_STATUS_OK;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, AVPS_STATUS_IO_ERROR, exception.what());
    } catch (const std::exception& exception) {
        return fail(error, AVPS_STATUS_IO_ERROR, exception.what());
    } catch (...) {
        return fail(error, AVPS_STATUS_IO_ERROR, "Unknown BIOS file error");
    }
}

template <typename Function>
AvpsStatus guarded(AvpsErrorBuffer* error, Function&& function) noexcept {
    const AvpsStatus error_status = validate_error_buffer(error);
    if (error_status != AVPS_STATUS_OK) {
        return error_status;
    }
    clear_error(error);
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return fail(error, AVPS_STATUS_OUT_OF_MEMORY, "Native allocation failed");
    } catch (const std::exception& exception) {
        return fail(error, AVPS_STATUS_INTERNAL_ERROR, exception.what());
    } catch (...) {
        return fail(error, AVPS_STATUS_INTERNAL_ERROR,
                    "Unknown native exception");
    }
}

struct SessionLease {
    std::shared_ptr<RegistrySlot> slot;
    std::shared_ptr<NativeSession> session;

    SessionLease() = default;
    SessionLease(const SessionLease&) = delete;
    SessionLease& operator=(const SessionLease&) = delete;

    SessionLease(SessionLease&& other) noexcept
        : slot(std::move(other.slot)), session(std::move(other.session)) {}

    SessionLease& operator=(SessionLease&& other) noexcept {
        if (this != &other) {
            release();
            slot = std::move(other.slot);
            session = std::move(other.session);
        }
        return *this;
    }

    void release() noexcept {
        if (!slot) {
            session.reset();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            if (slot->in_flight > 0) {
                --slot->in_flight;
            }
            if (slot->closing && slot->in_flight == 0) {
                slot->drained.notify_all();
            }
        }
        session.reset();
        slot.reset();
    }

    ~SessionLease() {
        release();
    }
};

AvpsStatus acquire_session(AvpsSessionHandle handle,
                           SessionLease& lease) noexcept {
    if (handle == 0) {
        return AVPS_STATUS_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = registry.find(handle);
    if (found == registry.end()) {
        return AVPS_STATUS_INVALID_HANDLE;
    }
    const auto& slot = found->second;
    if (slot->closing) {
        return AVPS_STATUS_HANDLE_CLOSING;
    }
    ++slot->in_flight;
    lease.slot = slot;
    lease.session = slot->session;
    return AVPS_STATUS_OK;
}

bool output_version_supported(uint32_t version) noexcept {
    return version_supported(version);
}

} // namespace

extern "C" {

AvpsStatus AVOPSEMU_CAPI_CALL
avps_get_abi_info(AvpsAbiInfo* out_info) {
    if (!out_info) {
        return AVPS_STATUS_INVALID_ARGUMENT;
    }

    const uint32_t caller_size = out_info->struct_size;
    constexpr uint32_t minimum_size =
        static_cast<uint32_t>(offsetof(AvpsAbiInfo, feature_bits));
    if (caller_size < minimum_size) {
        return AVPS_STATUS_STRUCT_TOO_SMALL;
    }

    out_info->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
    out_info->minimum_abi_version = abi_minimum_version;
    out_info->current_abi_version = abi_current_version;
    if (caller_size >= sizeof(AvpsAbiInfo)) {
        out_info->feature_bits =
            AVPS_FEATURE_SESSION_FOUNDATION | AVPS_FEATURE_SESSION_STATE |
            AVPS_FEATURE_CORE_LIFECYCLE | AVPS_FEATURE_DATA_SNAPSHOT |
            AVPS_FEATURE_INPUT_AUDIO_STATUS;
    }
    return AVPS_STATUS_OK;
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_create(const AvpsCreateInfo* info,
                    AvpsSessionHandle* out_handle,
                    AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (!info || !out_handle) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Create info and output handle are required");
        }
        *out_handle = 0;
        if (info->struct_size < sizeof(AvpsCreateInfo)) {
            return fail(error, AVPS_STATUS_STRUCT_TOO_SMALL,
                        "Create info struct is too small");
        }
        if (!version_supported(info->abi_version)) {
            return fail(error, AVPS_STATUS_ABI_MISMATCH,
                        "Requested ABI version is not supported");
        }
        if ((info->flags & ~AVPS_CREATE_FLAG_DISABLE_AUDIO) != 0 ||
            info->reserved != 0) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Unknown create flags or reserved values are nonzero");
        }

        SessionCreationReservation reservation;
        if (!reservation.acquire()) {
            return fail(error, AVPS_STATUS_SESSION_LIMIT,
                        "ABI v1 allows one live session per process");
        }

        auto session = std::make_shared<NativeSession>(
            (info->flags & AVPS_CREATE_FLAG_DISABLE_AUDIO) != 0);
        auto slot = std::make_shared<RegistrySlot>();
        slot->session = std::move(session);

        std::lock_guard<std::mutex> lock(registry_mutex);
        constexpr uint64_t max_safe_handle =
            static_cast<uint64_t>(std::numeric_limits<intptr_t>::max());
        if (next_handle == 0 || next_handle > max_safe_handle) {
            return fail(error, AVPS_STATUS_SESSION_LIMIT,
                        "Native session handle space is exhausted");
        }
        const AvpsSessionHandle handle = next_handle++;
        registry.emplace(handle, std::move(slot));
        *out_handle = handle;
        session_transition = false;
        reservation.active = false;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_destroy(AvpsSessionHandle handle, AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (handle == 0) {
            return fail(error, AVPS_STATUS_INVALID_HANDLE,
                        "Session handle is invalid");
        }

        std::shared_ptr<RegistrySlot> slot;
        std::shared_ptr<NativeSession> session;
        {
            std::unique_lock<std::mutex> lock(registry_mutex);
            const auto found = registry.find(handle);
            if (found == registry.end()) {
                return fail(error, AVPS_STATUS_INVALID_HANDLE,
                            "Session handle is stale or invalid");
            }
            slot = found->second;
            if (slot->closing) {
                return fail(error, AVPS_STATUS_HANDLE_CLOSING,
                            "Session is already being destroyed");
            }
            slot->closing = true;
            session_transition = true;
            try {
                slot->drained.wait(lock, [&slot] {
                    return slot->in_flight == 0;
                });
            } catch (...) {
                slot->closing = false;
                session_transition = false;
                throw;
            }
            session = std::move(slot->session);
            registry.erase(found);
        }

        if (session) {
            session->shutdown();
            session.reset();
        }
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            session_transition = false;
        }
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_get_state(AvpsSessionHandle handle,
                       AvpsSessionInfo* out_info,
                       AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (!out_info) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Session info output is required");
        }
        const uint32_t caller_size = out_info->struct_size;
        constexpr uint32_t minimum_size =
            static_cast<uint32_t>(offsetof(AvpsSessionInfo, last_error_code));
        if (caller_size < minimum_size) {
            return fail(error, AVPS_STATUS_STRUCT_TOO_SMALL,
                        "Session info struct is too small");
        }
        if (!output_version_supported(out_info->abi_version)) {
            return fail(error, AVPS_STATUS_ABI_MISMATCH,
                        "Session info ABI version is not supported");
        }

        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        std::lock_guard<std::mutex> lifecycle_lock(lease.session->lifecycle_mutex);
        lease.session->refresh_run_state();
        const uint32_t flags = session_flags(*lease.session);
        uint64_t instructions = 0;
        {
            std::lock_guard<std::mutex> machine_lock(lease.session->machine_mutex);
            instructions = lease.session->cpu->instructions;
        }

        out_info->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
        out_info->run_state = lease.session->run_state;
        out_info->flags = flags;
        if (caller_size >= offsetof(AvpsSessionInfo, last_error_code) +
                               sizeof(out_info->last_error_code)) {
            out_info->last_error_code = lease.session->last_error_code;
        }
        if (caller_size >= offsetof(AvpsSessionInfo, reserved) +
                               sizeof(out_info->reserved)) {
            out_info->reserved = 0;
        }
        if (caller_size >= offsetof(AvpsSessionInfo, instructions) +
                               sizeof(out_info->instructions)) {
            out_info->instructions = instructions;
        }
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_load_bios(AvpsSessionHandle handle,
                       const char* path_utf8,
                       AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (!path_utf8 || path_utf8[0] == '\0') {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "A non-empty UTF-8 BIOS path is required");
        }
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        try {
            session.stop_workers();
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Could not stop workers before BIOS load");
        }
        session.run_state = AVPS_STATE_STOPPED;

        std::vector<uint8_t> image;
        const AvpsStatus read_status = read_bios_image(path_utf8, image, error);
        if (read_status != AVPS_STATUS_OK) {
            session.last_error_code = read_status;
            return read_status;
        }

        try {
            std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
            ps1::boot_bios(*session.bus, *session.cpu, image);
            session.rebind_host_pad();
        } catch (const std::bad_alloc&) {
            return fail_session(session, error, AVPS_STATUS_OUT_OF_MEMORY,
                                "Could not allocate the BIOS machine state");
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Unknown failure while applying BIOS");
        }

        session.bios_ready = true;
        session.disc_mounted = session.bus->cd.sectors != 0;
        session.program_ready = false;
        session.direct_exe_boot = false;
        session.reset_entry = 0xbfc00000;
        session.reset_registers.fill(0);
        session.restore_reset_registers = false;
        session.last_error_code = AVPS_STATUS_OK;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_mount_disc(AvpsSessionHandle handle,
                        const char* path_utf8,
                        AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (!path_utf8 || path_utf8[0] == '\0') {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "A non-empty UTF-8 disc path is required");
        }
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        try {
            session.stop_workers();
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Could not stop workers before disc mount");
        }
        session.run_state = AVPS_STATE_STOPPED;

        std::filesystem::path path;
        try {
            path = std::filesystem::u8path(path_utf8);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_IO_ERROR,
                                exception.what());
        }
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error || !exists) {
            return fail_session(session, error, AVPS_STATUS_IO_ERROR,
                                "Disc path does not exist or cannot be accessed");
        }

        try {
            std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
            session.bus->cd.mount(path);
        } catch (const std::bad_alloc&) {
            return fail_session(session, error, AVPS_STATUS_OUT_OF_MEMORY,
                                "Could not allocate mounted disc state");
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_FORMAT_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_FORMAT_ERROR,
                                "Unknown disc format or mount failure");
        }

        session.disc_mounted = session.bus->cd.sectors != 0;
        session.last_error_code = AVPS_STATUS_OK;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_eject_disc(AvpsSessionHandle handle, AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        try {
            session.stop_workers();
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Could not stop workers before disc eject");
        }
        session.run_state = AVPS_STATE_STOPPED;
        {
            std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
            session.bus->cd.eject();
        }
        session.disc_mounted = false;
        session.last_error_code = AVPS_STATUS_OK;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_boot_disc(AvpsSessionHandle handle,
                       uint32_t boot_route,
                       AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (boot_route != AVPS_BOOT_ROUTE_BIOS &&
            boot_route != AVPS_BOOT_ROUTE_DIRECT_EXE) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Boot route is not supported");
        }
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        try {
            session.stop_workers();
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Could not stop workers before disc boot");
        }
        session.run_state = AVPS_STATE_STOPPED;

        if (!session.disc_mounted) {
            return fail_session(session, error, AVPS_STATUS_INVALID_STATE,
                                "Mount a disc before booting");
        }
        const bool direct =
            boot_route == AVPS_BOOT_ROUTE_DIRECT_EXE;
        if (!direct && !session.bios_ready) {
            return fail_session(session, error, AVPS_STATUS_INVALID_STATE,
                                "Load a valid BIOS before BIOS disc boot");
        }

        try {
            std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
            ps1::boot_disc(*session.bus, *session.cpu, direct);
            session.rebind_host_pad();
            session.reset_entry = session.cpu->pc;
            session.reset_registers = session.cpu->r;
            session.restore_reset_registers = direct;
            session.disc_mounted = session.bus->cd.sectors != 0;
        } catch (const std::bad_alloc&) {
            return fail_session(session, error, AVPS_STATUS_OUT_OF_MEMORY,
                                "Could not allocate boot machine state");
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_FORMAT_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_FORMAT_ERROR,
                                "Unknown disc boot failure");
        }

        session.program_ready = true;
        session.direct_exe_boot = direct;
        session.last_error_code = AVPS_STATUS_OK;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_start(AvpsSessionHandle handle, AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        session.refresh_run_state();
        if (session.run_state == AVPS_STATE_RUNNING) {
            return fail_session(session, error, AVPS_STATUS_INVALID_STATE,
                                "Session is already running");
        }
        if (session.run_state == AVPS_STATE_FAULTED) {
            return fail_session(session, error, AVPS_STATUS_CORE_FAULT,
                                "Reset or reinitialize the session after a Core fault");
        }

        {
            std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
            session.bus->spu.audio.clear();
            session.rebind_host_pad();
        }
        bool audio_available = false;
        if (!session.audio_disabled) {
            audio_available =
                session.audio_output.open(session.bus->spu,
                                          session.machine_mutex);
            if (!audio_available) {
                session.audio_output.close();
            }
        } else {
            session.audio_output.close();
        }

        try {
            session.runner = std::make_unique<ps1::Runner>(
                *session.cpu, session.machine_mutex);
            session.runner->start(true);
        } catch (const std::bad_alloc&) {
            if (session.runner) {
                session.runner.reset();
            }
            session.audio_output.close();
            session.run_state = AVPS_STATE_STOPPED;
            return fail_session(session, error, AVPS_STATUS_OUT_OF_MEMORY,
                                "Could not allocate the CPU Runner");
        } catch (const std::exception& exception) {
            if (session.runner) {
                session.runner.reset();
            }
            session.audio_output.close();
            session.run_state = AVPS_STATE_STOPPED;
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            if (session.runner) {
                session.runner.reset();
            }
            session.audio_output.close();
            session.run_state = AVPS_STATE_STOPPED;
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Could not start the CPU Runner");
        }

        session.run_state = AVPS_STATE_RUNNING;
        if (!audio_available) {
            session.last_error_code = AVPS_STATUS_WARNING_AUDIO_UNAVAILABLE;
            return fail(
                error, AVPS_STATUS_WARNING_AUDIO_UNAVAILABLE,
                session.audio_disabled
                    ? "Audio output is disabled by request; CPU is running"
                    : "CPU is running, but Windows audio output is unavailable");
        }
        session.last_error_code = AVPS_STATUS_OK;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_pause(AvpsSessionHandle handle, AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        session.refresh_run_state();
        if (session.run_state == AVPS_STATE_FAULTED) {
            return fail_session(session, error, AVPS_STATUS_CORE_FAULT,
                                "Core Runner stopped after a machine fault");
        }
        if (session.run_state != AVPS_STATE_RUNNING) {
            return fail_session(session, error, AVPS_STATUS_INVALID_STATE,
                                "Only a running session can be paused");
        }
        try {
            session.stop_workers();
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Could not stop workers while pausing");
        }
        session.run_state = AVPS_STATE_PAUSED;
        session.last_error_code = AVPS_STATUS_OK;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_stop(AvpsSessionHandle handle, AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        session.refresh_run_state();
        const bool faulted = session.run_state == AVPS_STATE_FAULTED;
        try {
            session.stop_workers();
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Could not stop session workers");
        }
        session.run_state = AVPS_STATE_STOPPED;
        if (!faulted) {
            session.last_error_code = AVPS_STATUS_OK;
        }
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_reset_cpu_keep_ram(AvpsSessionHandle handle,
                                AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        try {
            session.stop_workers();
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Could not stop workers before CPU reset");
        }
        {
            std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
            session.cpu->reset(session.reset_entry);
            if (session.restore_reset_registers) {
                session.cpu->r = session.reset_registers;
            }
        }
        session.run_state = AVPS_STATE_STOPPED;
        session.last_error_code = AVPS_STATUS_OK;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_reinitialize(AvpsSessionHandle handle, AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        try {
            session.stop_workers();
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Could not stop workers before reinitialize");
        }

        try {
            std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
            const auto bios_image = session.bus->bios;
            ps1::boot_bios(*session.bus, *session.cpu, bios_image);
            session.rebind_host_pad();
            session.disc_mounted = session.bus->cd.sectors != 0;
        } catch (const std::bad_alloc&) {
            return fail_session(session, error, AVPS_STATUS_OUT_OF_MEMORY,
                                "Could not allocate reinitialized machine state");
        } catch (const std::exception& exception) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                exception.what());
        } catch (...) {
            return fail_session(session, error, AVPS_STATUS_INTERNAL_ERROR,
                                "Unknown machine reinitialize failure");
        }

        session.program_ready = false;
        session.direct_exe_boot = false;
        session.reset_entry = 0xbfc00000;
        session.reset_registers.fill(0);
        session.restore_reset_registers = false;
        session.run_state = AVPS_STATE_STOPPED;
        session.last_error_code = AVPS_STATUS_OK;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_get_cpu_snapshot(AvpsSessionHandle handle,
                              AvpsCpuSnapshot* out_snapshot,
                              AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (!out_snapshot) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "CPU snapshot output is required");
        }
        const uint32_t caller_size = out_snapshot->struct_size;
        if (caller_size < sizeof(AvpsCpuSnapshot)) {
            return fail(error, AVPS_STATUS_STRUCT_TOO_SMALL,
                        "CPU snapshot struct is too small");
        }
        if (!output_version_supported(out_snapshot->abi_version)) {
            return fail(error, AVPS_STATUS_ABI_MISMATCH,
                        "CPU snapshot ABI version is not supported");
        }

        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
        const auto& cpu = *session.cpu;
        out_snapshot->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
        out_snapshot->pc = cpu.pc;
        out_snapshot->next_pc = cpu.next_pc;
        out_snapshot->hi = cpu.hi;
        out_snapshot->lo = cpu.lo;
        std::copy(cpu.r.begin(), cpu.r.end(), out_snapshot->gpr);
        std::copy(cpu.cop0.begin(), cpu.cop0.end(), out_snapshot->cop0);
        out_snapshot->instructions = cpu.instructions;
        out_snapshot->interrupts = cpu.interrupts;
        out_snapshot->reserved_exceptions = cpu.reserved_exceptions;
        out_snapshot->fault_valid = cpu.first_fault.valid ? 1u : 0u;
        out_snapshot->fault_code =
            cpu.first_fault.valid ? cpu.first_fault.code : 0u;
        out_snapshot->fault_pc =
            cpu.first_fault.valid ? cpu.first_fault.pc : 0u;
        out_snapshot->fault_opcode =
            cpu.first_fault.valid && cpu.first_fault.has_opcode
                ? cpu.first_fault.opcode
                : 0u;
        out_snapshot->fault_instruction =
            cpu.first_fault.valid ? cpu.first_fault.instruction : 0u;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_copy_vram_rgb555(AvpsSessionHandle handle,
                              AvpsVramInfo* out_info,
                              uint8_t* destination,
                              uint64_t capacity_bytes,
                              AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (!out_info) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "VRAM metadata output is required");
        }
        const uint32_t caller_size = out_info->struct_size;
        if (caller_size < sizeof(AvpsVramInfo)) {
            return fail(error, AVPS_STATUS_STRUCT_TOO_SMALL,
                        "VRAM metadata struct is too small");
        }
        if (!output_version_supported(out_info->abi_version)) {
            return fail(error, AVPS_STATUS_ABI_MISMATCH,
                        "VRAM metadata ABI version is not supported");
        }
        if (capacity_bytes != 0 && !destination) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "VRAM destination is null for a nonzero capacity");
        }

        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
        const auto& gpu = session.bus->gpu;
        constexpr uint32_t width = 1024;
        constexpr uint32_t height = 512;
        constexpr uint32_t stride_bytes = width * 2;
        constexpr uint64_t required_bytes =
            static_cast<uint64_t>(stride_bytes) * height;

        out_info->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
        out_info->width = width;
        out_info->height = height;
        out_info->stride_bytes = stride_bytes;
        out_info->pixel_format = AVPS_PIXEL_FORMAT_RGB555;
        out_info->required_bytes = required_bytes;
        out_info->video_frames = gpu.video_frames;
        out_info->display_revision = gpu.display_revision;

        if (capacity_bytes < required_bytes) {
            return fail(error, AVPS_STATUS_BUFFER_TOO_SMALL,
                        "VRAM destination buffer is too small");
        }
        if (!destination) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "VRAM destination is required");
        }

        for (size_t i = 0; i < gpu.vram.size(); ++i) {
            const uint16_t pixel = gpu.vram[i];
            destination[i * 2] = static_cast<uint8_t>(pixel);
            destination[i * 2 + 1] = static_cast<uint8_t>(pixel >> 8);
        }
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_copy_display_rgba8(AvpsSessionHandle handle,
                                AvpsVideoInfo* out_info,
                                uint8_t* destination,
                                uint64_t capacity_bytes,
                                AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (!out_info) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Display metadata output is required");
        }
        const uint32_t caller_size = out_info->struct_size;
        if (caller_size < sizeof(AvpsVideoInfo)) {
            return fail(error, AVPS_STATUS_STRUCT_TOO_SMALL,
                        "Display metadata struct is too small");
        }
        if (!output_version_supported(out_info->abi_version)) {
            return fail(error, AVPS_STATUS_ABI_MISMATCH,
                        "Display metadata ABI version is not supported");
        }
        if (capacity_bytes != 0 && !destination) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Display destination is null for a nonzero capacity");
        }

        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
        const auto& gpu = session.bus->gpu;
        const ps1::DisplayView view = ps1::display_view(gpu);
        const uint32_t stride_bytes = view.width * 4;
        const uint64_t required_bytes =
            static_cast<uint64_t>(stride_bytes) * view.height;

        out_info->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
        out_info->width = view.width;
        out_info->height = view.height;
        out_info->stride_bytes = stride_bytes;
        out_info->pixel_format = AVPS_PIXEL_FORMAT_RGBA8;
        out_info->flags =
            view.disabled ? AVPS_VIDEO_FLAG_DISABLED : 0;
        out_info->display_mode = gpu.display[3];
        out_info->source_x = view.x;
        out_info->source_y = view.y;
        out_info->reserved = 0;
        out_info->required_bytes = required_bytes;
        out_info->video_frames = gpu.video_frames;
        out_info->display_revision = gpu.display_revision;

        if (capacity_bytes < required_bytes) {
            return fail(error, AVPS_STATUS_BUFFER_TOO_SMALL,
                        "Display destination buffer is too small");
        }
        if (!destination) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Display destination is required");
        }

        for (uint32_t y = 0; y < view.height; ++y) {
            for (uint32_t x = 0; x < view.width; ++x) {
                const uint32_t rgb = ps1::display_pixel(gpu, view, x, y);
                const uint64_t offset =
                    static_cast<uint64_t>(y) * stride_bytes + x * 4;
                destination[offset] = static_cast<uint8_t>(rgb >> 16);
                destination[offset + 1] = static_cast<uint8_t>(rgb >> 8);
                destination[offset + 2] = static_cast<uint8_t>(rgb);
                destination[offset + 3] = 0xff;
            }
        }
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_set_pad_buttons(AvpsSessionHandle handle,
                             uint16_t buttons_active_low,
                             AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }
        lease.session->host_pad.publish(buttons_active_low);
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_release_pad(AvpsSessionHandle handle, AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }
        lease.session->host_pad.clear();
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_get_pad_status(AvpsSessionHandle handle,
                            AvpsInputStatus* out_status,
                            AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (!out_status) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Input status output is required");
        }
        const uint32_t caller_size = out_status->struct_size;
        if (caller_size < sizeof(AvpsInputStatus)) {
            return fail(error, AVPS_STATUS_STRUCT_TOO_SMALL,
                        "Input status struct is too small");
        }
        if (!output_version_supported(out_status->abi_version)) {
            return fail(error, AVPS_STATUS_ABI_MISMATCH,
                        "Input status ABI version is not supported");
        }
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        uint16_t buttons_active_low = 0xffff;
        uint16_t pending_presses = 0;
        lease.session->host_pad.snapshot(buttons_active_low, pending_presses);
        out_status->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
        out_status->buttons_active_low = buttons_active_low;
        out_status->pending_presses = pending_presses;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_set_audio_muted(AvpsSessionHandle handle,
                             uint32_t muted,
                             AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (muted > 1) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Muted must be zero or one");
        }
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }
        std::lock_guard<std::mutex> lifecycle_lock(
            lease.session->lifecycle_mutex);
        lease.session->audio_output.set_muted(muted != 0);
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

AvpsStatus AVOPSEMU_CAPI_CALL
avps_session_get_audio_status(AvpsSessionHandle handle,
                              AvpsAudioStatus* out_status,
                              AvpsErrorBuffer* error) {
    return guarded(error, [&]() -> AvpsStatus {
        if (!out_status) {
            return fail(error, AVPS_STATUS_INVALID_ARGUMENT,
                        "Audio status output is required");
        }
        const uint32_t caller_size = out_status->struct_size;
        if (caller_size < sizeof(AvpsAudioStatus)) {
            return fail(error, AVPS_STATUS_STRUCT_TOO_SMALL,
                        "Audio status struct is too small");
        }
        if (!output_version_supported(out_status->abi_version)) {
            return fail(error, AVPS_STATUS_ABI_MISMATCH,
                        "Audio status ABI version is not supported");
        }
        SessionLease lease;
        const AvpsStatus acquired = acquire_session(handle, lease);
        if (acquired != AVPS_STATUS_OK) {
            return fail(error, acquired, "Session handle is stale or unavailable");
        }

        auto& session = *lease.session;
        std::lock_guard<std::mutex> lifecycle_lock(session.lifecycle_mutex);
        const bool available = session.audio_output.ready();
        const bool muted = session.audio_output.muted_state();
        const bool mmcss = session.audio_output.multimedia_scheduled();
        const double ratio = session.audio_output.ratio();
        uint64_t core_dropped = 0;
        {
            std::lock_guard<std::mutex> machine_lock(session.machine_mutex);
            core_dropped = session.bus->spu.dropped_frames;
        }
        const uint64_t host_dropped = session.audio_output.dropped();
        const uint64_t dropped =
            host_dropped > std::numeric_limits<uint64_t>::max() - core_dropped
                ? std::numeric_limits<uint64_t>::max()
                : core_dropped + host_dropped;

        uint32_t flags = 0;
        if (available) {
            flags |= AVPS_AUDIO_FLAG_AVAILABLE;
        }
        if (muted) {
            flags |= AVPS_AUDIO_FLAG_MUTED;
        }
        if (mmcss) {
            flags |= AVPS_AUDIO_FLAG_MMCSS;
        }
        if (session.audio_disabled) {
            flags |= AVPS_AUDIO_FLAG_DISABLED_BY_REQUEST;
        }
        uint32_t tempo_milli = 0;
        if (std::isfinite(ratio)) {
            const double scaled = std::clamp(
                ratio * 1000.0, 0.0,
                static_cast<double>(std::numeric_limits<uint32_t>::max()));
            tempo_milli = static_cast<uint32_t>(scaled + 0.5);
        }

        out_status->abi_version = AVOPSEMU_CAPI_ABI_VERSION;
        out_status->flags = flags;
        out_status->native_error =
            static_cast<uint32_t>(session.audio_output.error());
        out_status->sample_rate_hz = 44100;
        out_status->channels = 2;
        out_status->bits_per_sample = 16;
        out_status->buffered_frames = session.audio_output.buffered_frames();
        out_status->tempo_milli = tempo_milli;
        out_status->reserved = 0;
        out_status->underruns = session.audio_output.underruns();
        out_status->dropped_frames = dropped;
        clear_error(error);
        return AVPS_STATUS_OK;
    });
}

} // extern "C"
