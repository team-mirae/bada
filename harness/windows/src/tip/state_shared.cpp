#include "state_shared.h"

#include <sddl.h>

#include "guids.h"

namespace bada::tip {
namespace {

// "D:P(A;;GA;;;SY)(A;;GA;;;<user>)S:(ML;;NWNR;;;ME)": SYSTEM and the user, medium integrity and up.
std::wstring Descriptor() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    DWORD size = 0;
    std::wstring sddl;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &sid)) {
            sddl = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;") + sid + L")S:(ML;;NWNR;;;ME)";
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return sddl;
}

// Whether the thread that published a state is still there.
bool ThreadAlive(DWORD thread_id, DWORD process_id) {
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
    if (!thread) return GetLastError() == ERROR_ACCESS_DENIED;  // it exists: an elevated process's
    DWORD code = 0;
    const bool alive =
        GetExitCodeThread(thread, &code) && code == STILL_ACTIVE && GetProcessIdOfThread(thread) == process_id;
    CloseHandle(thread);
    return alive;
}

// The writers' turn. A writer that died holding it leaves it abandoned, which is a turn too.
class Turn {
public:
    explicit Turn(HANDLE lock) : lock_(lock) {
        const DWORD waited = lock_ ? WaitForSingleObject(lock_, 50) : WAIT_FAILED;
        held_ = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }
    ~Turn() {
        if (held_) ReleaseMutex(lock_);
    }
    bool held() const { return held_; }

private:
    HANDLE lock_;
    bool held_ = false;
};

}  // namespace

SharedState::SharedState(const wchar_t* name, const wchar_t* lock) : name_(name), lock_name_(lock) {}

SharedState::~SharedState() {
    if (block_) UnmapViewOfFile(block_);
    if (mapping_) CloseHandle(mapping_);
    if (lock_) CloseHandle(lock_);
}

bool SharedState::Open() {
    if (tried_) return block_ != nullptr;
    tried_ = true;
    const std::wstring sddl = Descriptor();
    if (sddl.empty()) return false;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) return false;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    lock_ = CreateMutexExW(&sa, lock_name_.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, static_cast<DWORD>(state::kBlockSize),
                                  name_.c_str());
    LocalFree(sd);
    if (!mapping_ || !lock_) return false;
    block_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, state::kBlockSize);
    if (!block_) return false;
    const Turn turn(lock_);
    if (turn.held()) state::Init(block_, state::kBlockSize);
    return true;
}

void SharedState::Publish(const std::string& json, const StateSummary& summary) {
    if (!Open()) return;
    const DWORD thread = GetCurrentThreadId(), process = GetCurrentProcessId();
    const std::uint32_t flags = state::Flags(summary);
    bool changed = false;
    {
        const Turn turn(lock_);
        if (!turn.held()) return;
        state::Init(block_, state::kBlockSize);
        const auto* header = static_cast<const state::Header*>(block_);
        const bool mine = header->thread_id == thread && header->process_id == process;
        // Focus left this thread, and another thread's field has it now: that one shows.
        if (!summary.active && !mine && header->length && (header->flags & state::kFlagActive) &&
            ThreadAlive(header->thread_id, header->process_id)) {
            return;
        }
        const bool told = header->length && header->flags == flags && mine;
        state::Wrote wrote = state::Write(block_, json, flags, process, thread);
        // Too long for the block (a menu of very many rows): without the rows.
        if (wrote == state::Wrote::kTooLong) wrote = state::Write(block_, state::WithoutRows(json), flags, process, thread);
        if (wrote == state::Wrote::kTooLong) wrote = state::Write(block_, {}, flags, process, thread);
        changed = wrote == state::Wrote::kWritten && !told;
    }
    // Programs that don't poll hear of the changes the flags carry. Typing within a composition
    // posts nothing: the text is in the block.
    if (changed) PostMessageW(HWND_BROADCAST, StateMessage(), thread, static_cast<LPARAM>(flags));
}

void SharedState::Clear() {
    if (!Open()) return;
    const DWORD thread = GetCurrentThreadId(), process = GetCurrentProcessId();
    const Turn turn(lock_);
    if (!turn.held()) return;
    const auto* header = static_cast<const state::Header*>(block_);
    if (header->thread_id != thread || header->process_id != process || !header->length) return;
    state::Write(block_, {}, 0, process, thread);
    PostMessageW(HWND_BROADCAST, StateMessage(), thread, 0);
}

SharedState& StateSinkOfProcess() {
    // Never destroyed: text services may publish until the process ends.
    static SharedState* sink = new SharedState(kStateMapping, kStateLock);
    return *sink;
}

UINT StateMessage() {
    static const UINT message = RegisterWindowMessageW(kStateMessage);
    return message;
}

std::optional<state::Snapshot> ReadSharedState(const wchar_t* name) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!mapping) return std::nullopt;
    std::optional<state::Snapshot> snapshot;
    if (const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, state::kBlockSize)) {
        snapshot = state::Read(view, state::kBlockSize);
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return snapshot;
}

}  // namespace bada::tip
