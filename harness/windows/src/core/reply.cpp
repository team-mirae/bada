#include "reply.h"

#include <algorithm>

#include "engine.h"

namespace bada {

size_t Scalars(const std::string& text) {
    size_t n = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

size_t ByteOffset(const std::string& text, size_t n) {
    size_t i = 0;
    while (i < text.size() && n > 0) {
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
        --n;
    }
    return i;
}

Reply ParseReply(const std::string& text) {
    Reply r;
    auto parsed = json::Parse(text);
    const json::Value* ok = parsed ? parsed->Get("ok") : nullptr;
    if (!ok || !ok->IsBool()) {
        r.error_code = "bad-event";
        r.error_message = "the reply isn't a JSON object with ok";
        return r;
    }
    const json::Value& v = *parsed;
    r.ok = ok->boolean;
    if (const json::Value* e = v.Get("error")) {
        r.error_code = e->GetString("code");
        r.error_message = e->GetString("message");
        r.error_path = e->GetString("path");
    }
    if (!r.ok && r.error_code.empty()) r.error_code = "bad-event";
    r.commit = v.GetString("commit");
    r.replace_selection = v.GetString("replace", "marked") == "selection";
    r.consumed = v.GetBool("consumed");
    if (const json::Value* t = v.Get("types"); t && t->IsString()) r.types = t->string;
    if (const json::Value* p = v.Get("preedit"); p && p->IsObject()) {
        r.preedit.text = p->GetString("text");
        const size_t length = Scalars(r.preedit.text);
        const json::Value* caret = p->Get("caret");
        r.preedit.caret = caret && caret->IsNumber() && caret->number >= 0 && caret->number <= static_cast<double>(length)
                              ? static_cast<size_t>(caret->number)
                              : length;
        if (const json::Value* segments = p->Get("segments"); segments && segments->IsArray()) {
            for (const json::Value& s : segments->array) {
                Segment seg;
                seg.len = static_cast<size_t>(s.GetNumber("len", 0));
                const std::string kind = s.GetString("kind", "live");
                seg.kind = kind == "fixed" ? Segment::Kind::kFixed
                           : kind == "picked" ? Segment::Kind::kPicked
                                              : Segment::Kind::kLive;
                r.preedit.segments.push_back(seg);
            }
        }
    }
    if (const json::Value* m = v.Get("menu"); m && m->IsObject()) {
        if (const json::Value* rows = m->Get("rows"); rows && rows->IsArray()) {
            for (const json::Value& item : rows->array) {
                Row row;
                row.text = item.GetString("text");
                row.entry = static_cast<int>(item.GetNumber("entry", 0));
                row.note = item.GetString("note");
                row.lang = item.GetString("lang");
                row.layout = item.GetString("layout");
                r.menu.rows.push_back(std::move(row));
            }
        }
        if (const json::Value* selected = m->Get("selected"); selected && selected->IsNumber() &&
                                                               selected->number >= 0 &&
                                                               selected->number < static_cast<double>(r.menu.rows.size())) {
            r.menu.selected = static_cast<size_t>(selected->number);
        }
        r.menu.strip = m->GetBool("strip");
    }
    if (const json::Value* mode = v.Get("mode"); mode && mode->IsString()) r.mode = mode->string;
    if (const json::Value* profile = v.Get("profile"); profile && profile->IsString()) r.profile = profile->string;
    r.staged = v.GetString("staged");
    r.preedit.is_staged = r.staged == r.preedit.text;
    if (const json::Value* ask = v.Get("ask"); ask && ask->IsObject()) {
        Ask a;
        if (const json::Value* selection = ask->Get("selection"); selection && !selection->IsNull()) {
            a.selection = true;
            if (selection->IsString()) a.selection_for = selection->string;
        }
        if (const json::Value* around = ask->Get("around"); around && around->IsObject()) {
            a.around = true;
            a.before = static_cast<size_t>(std::max(0.0, around->GetNumber("before", 0)));
            a.after = static_cast<size_t>(std::max(0.0, around->GetNumber("after", 0)));
        }
        if (a.selection || a.around) r.ask = a;
    }
    r.rtl = v.GetString("dir") == "rtl";
    r.raw = std::move(*parsed);
    return r;
}

void ApplyOnce(const Reply& r, Field& field) {
    if (!r.ok) {
        // Nothing changed: what the last reply showed stays. After a panic the engine has reset
        // itself, so the marked text and the menu go.
        if (r.error_code == "panic") {
            field.Begin();
            field.SetPreedit(Preedit{});
            field.ShowMenu(Menu{}, false);
            field.End();
        }
        return;
    }
    field.Begin();
    if (!r.commit.empty()) field.Commit(r.commit, r.replace_selection);
    field.SetPreedit(r.preedit);
    field.ShowMenu(r.menu, r.rtl);
    field.End();
}

Reply Apply(const Reply& r, Field& field, const std::function<Reply(const std::string& event_json)>& send) {
    ApplyOnce(r, field);
    if (!r.ok || !r.ask) return r;
    // The engine needs text to finish: one `text` event, whose reply is applied and not asked again.
    const Around around = field.TextAround(r.ask->around ? r.ask->before : 0, r.ask->around ? r.ask->after : 0);
    Reply answer = send(TextEventJson(around.selection, r.ask->around, around.before, around.after));
    ApplyOnce(answer, field);
    // A refused answer changed nothing: the first reply still shows.
    return answer.ok ? answer : r;
}

}  // namespace bada
