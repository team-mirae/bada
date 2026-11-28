#include "tide_ipc.h"

#include <atomic>
#include <cstring>

namespace bada::tide::ipc {
namespace {

// As state_block.cpp: lock-free 32-bit atomics are address-free, so they work across processes.
std::uint32_t* Word(void* block, std::size_t i) { return static_cast<std::uint32_t*>(block) + i; }
std::uint32_t Load(const void* block, std::size_t i) {
    return std::atomic_ref<std::uint32_t>(*const_cast<std::uint32_t*>(static_cast<const std::uint32_t*>(block) + i))
        .load(std::memory_order_acquire);
}
constexpr std::size_t kHeader = 8;

}  // namespace

bool WriteStatus(void* block, std::string_view json) {
    if (!block || json.size() > kStatusSize - kHeader) return false;
    std::atomic_ref<std::uint32_t> sequence(*Word(block, 0));
    std::uint32_t s = sequence.load(std::memory_order_relaxed);
    if (s & 1) ++s;  // a writer died in the middle: start clean
    sequence.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(static_cast<char*>(block) + kHeader, json.data(), json.size());
    std::atomic_ref<std::uint32_t>(*Word(block, 1)).store(static_cast<std::uint32_t>(json.size()), std::memory_order_relaxed);
    sequence.store(s + 2, std::memory_order_release);
    return true;
}

std::optional<std::string> ReadStatus(const void* block) {
    if (!block) return std::nullopt;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const std::uint32_t before = Load(block, 0);
        if (before & 1) continue;
        std::uint32_t length = Load(block, 1);
        if (length > kStatusSize - kHeader) length = 0;
        std::string text(static_cast<const char*>(block) + kHeader, length);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (Load(block, 0) != before) continue;
        if (text.empty()) return std::nullopt;
        return text;
    }
    return std::nullopt;
}

}  // namespace bada::tide::ipc
