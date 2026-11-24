#include "state_block.h"

#include <atomic>
#include <cstring>

#include "json.h"

namespace bada::state {
namespace {

// Fields shared between processes are read and written atomically: lock-free 32-bit atomics are
// address-free, so they work across processes on one mapping.
std::atomic_ref<std::uint32_t> At(std::uint32_t& field) { return std::atomic_ref<std::uint32_t>(field); }
std::uint32_t Load(const std::uint32_t& field) {
    return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t&>(field)).load(std::memory_order_acquire);
}

char* Text(void* block) { return static_cast<char*>(block) + sizeof(Header); }
const char* Text(const void* block) { return static_cast<const char*>(block) + sizeof(Header); }

}  // namespace

std::uint32_t Flags(const StateSummary& s) {
    return (s.mode == Mode::kHenkan ? kFlagHenkan : 0) | (s.active ? kFlagActive : 0) | (s.secure ? kFlagSecure : 0) |
           (s.composing ? kFlagComposing : 0) | (s.caps_lock ? kFlagCapsLock : 0);
}

void Init(void* block, std::size_t size) {
    auto* h = static_cast<Header*>(block);
    if (size <= sizeof(Header) || Load(h->magic) == kMagic) return;
    h->version = kVersion;
    h->capacity = static_cast<std::uint32_t>(size - sizeof(Header));
    At(h->magic).store(kMagic, std::memory_order_release);
}

Wrote Write(void* block, std::string_view json, std::uint32_t flags, std::uint32_t process_id,
            std::uint32_t thread_id) {
    auto* h = static_cast<Header*>(block);
    // One byte is the NUL after the text.
    if (json.size() + 1 > h->capacity) return Wrote::kTooLong;
    const auto length = static_cast<std::uint32_t>(json.size());
    if (Load(h->length) == length && Load(h->flags) == flags && Load(h->thread_id) == thread_id &&
        Load(h->process_id) == process_id && std::memcmp(Text(block), json.data(), length) == 0) {
        return Wrote::kSame;
    }
    auto sequence = At(h->sequence);
    std::uint32_t s = sequence.load(std::memory_order_relaxed);
    if (s & 1) ++s;  // a writer died in the middle: start clean
    sequence.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    At(h->flags).store(flags, std::memory_order_relaxed);
    At(h->process_id).store(process_id, std::memory_order_relaxed);
    At(h->thread_id).store(thread_id, std::memory_order_relaxed);
    std::memcpy(Text(block), json.data(), length);
    Text(block)[length] = '\0';
    At(h->length).store(length, std::memory_order_relaxed);
    sequence.store(s + 2, std::memory_order_release);
    return Wrote::kWritten;
}

std::optional<Snapshot> Read(const void* block, std::size_t size) {
    const auto* h = static_cast<const Header*>(block);
    if (!block || size <= sizeof(Header) || Load(h->magic) != kMagic) return std::nullopt;
    const std::size_t capacity = size - sizeof(Header);
    for (int attempt = 0; attempt < 64; ++attempt) {
        const std::uint32_t before = Load(h->sequence);
        if (before & 1) continue;
        Snapshot s;
        s.flags = Load(h->flags);
        s.process_id = Load(h->process_id);
        s.thread_id = Load(h->thread_id);
        std::uint32_t length = Load(h->length);
        if (length > capacity) length = 0;
        s.json.assign(Text(block), length);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (Load(h->sequence) != before) continue;
        if (s.json.empty()) return std::nullopt;
        return s;
    }
    return std::nullopt;
}

std::string WithoutRows(const std::string& text) {
    auto v = json::Parse(text);
    if (!v || !v->IsObject()) return {};
    json::Value menu = json::Object();
    menu.Set("rows", json::Array()).Set("selected", json::Null()).Set("strip", json::Bool(false));
    v->Set("menu", std::move(menu));
    return json::Write(*v);
}

}  // namespace bada::state
