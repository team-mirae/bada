// The published state's place on Windows (docs/bada-compatible-v2.md §6.7, §12): a block of
// shared memory every program of the session can read (src/core/state_block.h has its layout),
// and a registered window message that says it changed.
#pragma once

#include <windows.h>

#include <optional>
#include <string>

#include "harness.h"
#include "state_block.h"

namespace bada::tip {

// Writes the state for other programs. The block is created at the first write and stays mapped
// for the life of the process. It is readable and writable by the user's own processes at medium
// integrity and above, and by nobody else: it holds what is being typed. So a sandboxed program
// (a Store app, a browser's renderer) can't read it, and the text service inside one can't publish.
class SharedState final : public StateSink {
public:
    // `name` and `lock`: the mapping's and its lock's names (guids.h; the tests use their own).
    SharedState(const wchar_t* name, const wchar_t* lock);
    ~SharedState() override;
    SharedState(const SharedState&) = delete;
    SharedState& operator=(const SharedState&) = delete;

    // Writes it, and posts the state message when what the message carries changed. A state with
    // no field in focus isn't written over another thread's that has one: the field that has
    // focus is the one that shows.
    void Publish(const std::string& json, const StateSummary& summary) override;
    // Publishes nothing: what this thread published is taken away.
    void Clear() override;

private:
    bool Open();
    std::wstring name_, lock_name_;
    HANDLE mapping_ = nullptr;
    HANDLE lock_ = nullptr;
    void* block_ = nullptr;
    bool tried_ = false;
};

// The process's, on the names of guids.h.
SharedState& StateSinkOfProcess();

// The registered message "com.teammirae.bada.state": posted to every top-level window when the
// mode, the focus, a password field, whether something is staged, or the lock changed. wParam: the
// thread that published. lParam: the flags of state_block.h. For the text, read the block.
UINT StateMessage();

// Reads the state as another program does: the mapping opened by name, read-only, for this call.
// nullopt: there is no mapping, or nothing is published.
std::optional<state::Snapshot> ReadSharedState(const wchar_t* name);

}  // namespace bada::tip
