// The published state's shared memory (docs/bada-compatible-v2.md §6.7, §12): one block, named
// Local\com.teammirae.bada.state.v2.{CLSID}, holding the state of the text field in focus as JSON.
// Any program of the session reads it with the system's own calls: no library, no plugin.
//
//   offset  size  field
//        0     4  magic        "BDS2"
//        4     4  version      2
//        8     4  capacity     bytes there are for the text
//       12     4  sequence     odd while the text is being written
//       16     4  length       bytes of the text now; 0: nothing is published
//       20     4  process_id   who published it
//       24     4  thread_id
//       28     4  flags        kFlag*, below
//       32     …  text         UTF-8 JSON (`bada-compatible-state/2`), then a NUL
//
// All fields little-endian. A reader reads `sequence`, copies the rest, reads `sequence` again, and
// starts over if it was odd or changed. Writers take turns (a named mutex, src/tip/state_shared.h).
// The layout only ever grows at its end. No Windows headers here: the tests run anywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "harness.h"

namespace bada::state {

inline constexpr std::uint32_t kMagic = 0x32534442;  // "BDS2"
inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::size_t kBlockSize = 65536;

// What the state message carries in its lParam, and `flags` holds.
inline constexpr std::uint32_t kFlagHenkan = 1;     // the mode is henkan
inline constexpr std::uint32_t kFlagActive = 2;     // a text field has focus
inline constexpr std::uint32_t kFlagSecure = 4;     // a password field: nothing typed is published
inline constexpr std::uint32_t kFlagComposing = 8;  // something is staged
inline constexpr std::uint32_t kFlagCapsLock = 16;  // the lock is on

struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t capacity;
    std::uint32_t sequence;
    std::uint32_t length;
    std::uint32_t process_id;
    std::uint32_t thread_id;
    std::uint32_t flags;
};
static_assert(sizeof(Header) == 32);

std::uint32_t Flags(const StateSummary& summary);

// Fills in the header of a zeroed block. Idempotent.
void Init(void* block, std::size_t size);

enum class Wrote { kWritten, kSame, kTooLong };
// Writes the state. One writer at a time: the caller holds the writers' lock. kSame: the block
// already holds this text and these flags from this thread, and nothing was written. An empty text
// publishes nothing.
Wrote Write(void* block, std::string_view json, std::uint32_t flags, std::uint32_t process_id,
            std::uint32_t thread_id);

struct Snapshot {
    std::string json;
    std::uint32_t flags = 0;
    std::uint32_t process_id = 0;
    std::uint32_t thread_id = 0;
};
// Reads the state. nullopt when the block isn't one, nothing is published, or it kept changing.
std::optional<Snapshot> Read(const void* block, std::size_t size);

// `json` with its menu's rows taken out, for a state too long for the block.
std::string WithoutRows(const std::string& json);

}  // namespace bada::state
