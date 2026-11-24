// The published state (docs/bada-compatible-v2.md §6.7, §12): `bada-compatible-state/2`, as a D-Bus
// signal on the session bus and as a file, each only when the JSON changed. Both are read with
// standard tools (gdbus, busctl, cat, jq), and nothing a reader does comes back to the harness.
//
//   signal  com.teammirae.Bada.State.Changed(s json)  at /com/teammirae/Bada
//   file    $XDG_RUNTIME_DIR/bada/state.json           (else /tmp/bada-<uid>/state.json)
#pragma once

#include <filesystem>
#include <string>

#include "harness.h"

namespace bada {

inline constexpr const char* kStateObject = "/com/teammirae/Bada";
inline constexpr const char* kStateInterface = "com.teammirae.Bada.State";

class Publisher final : public StateSink {
public:
    Publisher();
    ~Publisher() override;
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    void Publish(const std::string& json) override;
    void Clear() override;

    static std::filesystem::path StatePath();

private:
    void Signal(const std::string& json);

    std::filesystem::path path_;
    std::string last_;
    void* bus_ = nullptr;  // DBusConnection*, private; null when there is no session bus
    bool bus_tried_ = false;
};

}  // namespace bada
