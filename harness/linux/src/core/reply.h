// A reply (docs/bada-compatible-v2.md §6), and how one is applied to a text field (§6.3). The
// text field is the frontend's: an input context in Fcitx5 or IBus, a plain model in the tests.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "json.h"

namespace bada {

struct Segment {
    size_t len = 0;  // in Unicode scalar values
    enum class Kind { kLive, kFixed, kPicked } kind = Kind::kLive;
};

struct Preedit {
    std::string text;
    size_t caret = 0;  // in Unicode scalar values; the end when the reply has none
    std::vector<Segment> segments;
    // The reply's `staged` is this text: a flush would commit exactly what shows. Frameworks that
    // commit the marked text themselves when focus leaves are told so.
    bool is_staged = true;
};

struct Row {
    std::string text;
    int entry = 0;  // 0 the top reading, 1 and up the others, -1 and down a layout's forms
    std::string note;
    std::string lang;
    std::string layout;
};

struct Menu {
    std::vector<Row> rows;
    std::optional<size_t> selected;  // the highlighted row, if any
    bool strip = false;              // one row of single characters
};

// What the engine needs from the harness to finish (`ask`).
struct Ask {
    bool selection = false;      // {"selection": "<what for>"}
    std::string selection_for;
    bool around = false;         // {"around": {"before": n, "after": n}}
    size_t before = 0, after = 0;
};

struct Reply {
    bool ok = false;
    std::string error_code, error_message, error_path;  // only with ok false
    std::string commit;
    bool replace_selection = false;  // `replace` is "selection"
    bool consumed = false;
    std::optional<std::string> types;  // nullopt: null, the engine has no say
    Preedit preedit;
    Menu menu;
    std::optional<std::string> mode, profile;  // as last selected; nullopt: null
    std::string staged;
    std::optional<Ask> ask;
    bool rtl = false;
    json::Value raw;  // the whole reply
};

// Reads a reply. Text that isn't a JSON object with `ok` reads as ok false, code "bad-event".
Reply ParseReply(const std::string& text);

// The number of Unicode scalar values in UTF-8 `text`, and the byte offset of the `n`th.
size_t Scalars(const std::string& text);
size_t ByteOffset(const std::string& text, size_t n);

// The text around the caret, for a reply's `ask`.
struct Around {
    std::string selection;  // "" when nothing is selected, or something is marked
    std::string before, after;
};

// The text field, as §6.3 needs it: a list of characters, a selection and an optional marked range.
class Field {
public:
    virtual ~Field() = default;
    // Replaces the marked text with `text` (the selection when nothing is marked, or when
    // `replace_selection`); the caret goes after it and nothing is marked.
    virtual void Commit(const std::string& text, bool replace_selection) = 0;
    // The marked text becomes `preedit` (it replaces the selection when nothing was marked), with
    // the caret at preedit.caret. An empty text deletes the marked text.
    virtual void SetPreedit(const Preedit& preedit) = 0;
    // Shows the menu; one with no rows closes it.
    virtual void ShowMenu(const Menu& menu, bool rtl) = 0;
    // The selection, and at most `before` and `after` characters around the caret.
    virtual Around TextAround(size_t before, size_t after) = 0;
};

// §6.3's apply(r): the reply, then its `ask`, answered once with a `text` event through `send`
// and never deeper. Returns the last reply applied: what shows now.
Reply Apply(const Reply& reply, Field& field, const std::function<Reply(const std::string& event_json)>& send);
// The same without the `ask` step: for a reply that already answers one.
void ApplyOnce(const Reply& reply, Field& field);

}  // namespace bada
