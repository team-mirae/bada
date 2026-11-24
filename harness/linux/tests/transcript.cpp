// Replays conformance transcripts (tools/conformance/README.md, `bada-transcript/1`) on the linked
// engine, through this harness's own engine wrapper (src/core/engine) and its reply-applying code
// (src/core/reply) on a plain text field (tests/textmodel.h).
//
// Each line's `expect` is checked against the reply as the conformance tool checks it. Each reply
// to a text field's event is also applied to that field's model as §6.3 says, and the model is
// checked: what is marked is the reply's preedit, the caret is where the reply puts it, and the
// rest of the text is what was committed.
//
//   bada_transcript <transcript.jsonl>...
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#include "check.h"
#include "engine.h"
#include "json.h"
#include "reply.h"
#include "textmodel.h"

using namespace bada;
using test::Check;

namespace {

// Where `got` differs from what is expected of it; "" when it doesn't. An expected object names
// only the fields that matter, an expected array is the whole array, and null also matches a
// missing field.
std::string Differs(const json::Value& expect, const json::Value& got, const std::string& path) {
    if (expect.IsObject() && got.IsObject()) {
        for (const auto& [key, value] : expect.object) {
            const std::string at = path.empty() ? key : path + "." + key;
            const json::Value* have = got.Get(key);
            if (!have) {
                if (value.IsNull()) continue;
                return at + ": missing, expected " + json::Write(value);
            }
            const std::string d = Differs(value, *have, at);
            if (!d.empty()) return d;
        }
        return "";
    }
    if (expect.IsArray() && got.IsArray()) {
        if (expect.array.size() != got.array.size()) {
            return path + ": " + std::to_string(got.array.size()) + " items, expected " + std::to_string(expect.array.size());
        }
        for (size_t i = 0; i < expect.array.size(); ++i) {
            const std::string d = Differs(expect.array[i], got.array[i], path + "[" + std::to_string(i) + "]");
            if (!d.empty()) return d;
        }
        return "";
    }
    if (json::Equal(expect, got)) return "";
    return path + ": " + json::Write(got) + ", expected " + json::Write(expect);
}

// One text field of a transcript: its engine, and the model its replies are applied to.
struct FieldUnderTest {
    std::unique_ptr<Engine> engine;
    test::TextModel model;
    std::string committed;  // everything committed so far, as it should stand in the text
    bool asked = false;     // the last reply asked for text: the next `text` event answers it
    int applied = 0;
};

int Run(const char* path, const EngineData& data) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "%s: can't be read\n", path);
        return 2;
    }
    std::unique_ptr<EngineSession> session;
    std::map<std::string, FieldUnderTest> fields;
    int lines = 0, applied = 0;
    const int failures_before = test::failures;
    std::string text;
    for (int n = 1; std::getline(in, text); ++n) {
        if (text.find_first_not_of(" \t\r") == std::string::npos) continue;
        ++lines;
        const std::string where = std::string(path) + ":" + std::to_string(n);
        const auto parsed = json::Parse(text);
        if (!parsed || !parsed->IsObject()) {
            Check(false, where + ": not JSON");
            continue;
        }
        const json::Value& line = *parsed;
        if (const json::Value* init = line.Get("init")) {
            fields.clear();
            std::string error;
            session = EngineSession::Create(&data, json::Write(*init), &error);
            Check(session != nullptr, where + ": init: " + error);
            continue;
        }
        if (const json::Value* expect = line.Get("describe")) {
            Describe d;
            std::string error;
            Check(ReadDescribe(&d, &error), where + ": describe: " + error);
            const std::string differs = Differs(*expect, d.raw, "");
            Check(differs.empty(), where + ": describe: " + differs);
            continue;
        }
        if (const json::Value* name = line.Get("new")) {
            if (!session) {
                Check(false, where + ": no session: a transcript starts with init");
                continue;
            }
            FieldUnderTest& f = fields[name->string];
            f.engine = std::make_unique<Engine>(*session);
            Check(f.engine->ok(), where + ": bada_new answered NULL");
            continue;
        }
        if (const json::Value* name = line.Get("free")) {
            const auto it = fields.find(name->string);
            Check(it != fields.end(), where + ": no engine named " + name->string);
            if (it != fields.end()) {
                std::printf("  %s: field %s ends as \"%s\" (%d replies applied)\n", path, name->string.c_str(),
                            it->second.model.Text().c_str(), it->second.applied);
                fields.erase(it);
            }
            continue;
        }
        const json::Value* send = line.Get("send");
        const json::Value* raw = line.Get("send_raw");
        if (!send && !(raw && raw->IsString())) {
            Check(line.Get("note") || line.Get("transcript"),
                  where + ": a line has init, describe, new, free, send, send_raw or note");
            continue;
        }
        if (!session) {
            Check(false, where + ": no session: a transcript starts with init");
            continue;
        }
        const std::string event = send ? json::Write(*send) : raw->string;
        const json::Value* to = line.Get("to");
        FieldUnderTest* field = nullptr;
        if (to) {
            const auto it = fields.find(to->string);
            Check(it != fields.end(), where + ": no engine named " + to->string);
            if (it == fields.end()) continue;
            field = &it->second;
        }

        // A `text` event answers the reply before it: put what it says is selected in the field.
        const bool answers = field && field->asked && send && send->GetString("type") == "text";
        if (answers) field->model.Select(send->GetString("selection"));

        const std::string reply_text = field ? field->engine->Send(event) : session->Send(event);
        const auto got = json::Parse(reply_text);
        Check(got.has_value() && got->IsObject(), where + ": the reply isn't a JSON object: " + reply_text);
        if (!got) continue;
        if (const json::Value* expect = line.Get("expect")) {
            const std::string differs = Differs(*expect, *got, "");
            Check(differs.empty(), where + ": " + differs + "\n      reply: " + reply_text);
        }
        if (!field) continue;

        // Apply it, as the harness does (§6.3). The transcript sends the `text` event itself, so
        // the `ask` step is left to it.
        const Reply reply = ParseReply(reply_text);
        const std::string before = field->model.Text();
        ApplyOnce(reply, field->model);
        ++field->applied;
        ++applied;
        test::TextModel& m = field->model;
        if (!reply.ok) {
            Check(m.Text() == before, where + ": a refused event changed the text");
            continue;
        }
        if (!reply.commit.empty()) {
            // A commit replaces the marked text, or the selection: here, what the `text` event selected.
            field->committed += reply.commit;
        } else if (answers) {
            field->committed += send->GetString("selection");  // nothing replaced it: it stays
        }
        Check(m.Marked() == reply.preedit.text,
              where + ": marked \"" + m.Marked() + "\", the reply's preedit is \"" + reply.preedit.text + "\"");
        Check(m.Text() == field->committed + reply.preedit.text,
              where + ": the text is \"" + m.Text() + "\", expected \"" + field->committed + reply.preedit.text + "\"");
        if (!reply.preedit.text.empty()) {
            Check(m.from == m.to && m.from == m.mark_from + reply.preedit.caret, where + ": the caret isn't at preedit.caret");
            size_t length = 0;
            for (const Segment& s : reply.preedit.segments) length += s.len;
            Check(reply.preedit.segments.empty() || length == Scalars(reply.preedit.text),
                  where + ": the segments don't add up to the preedit");
        } else {
            Check(!m.marked && m.from == m.to && m.from == m.text.size(), where + ": the caret isn't after the text");
        }
        Check(m.menu.rows.size() == (reply.raw.Get("menu") && reply.raw.Get("menu")->Get("rows")
                                         ? reply.raw.Get("menu")->Get("rows")->array.size()
                                         : 0),
              where + ": the menu shown isn't the reply's");
        field->asked = reply.ask.has_value();
    }
    const int failed = test::failures - failures_before;
    std::printf("%s %s: %d lines, %d replies applied, %d failed checks\n", failed ? "FAIL" : "ok  ", path, lines, applied,
                failed);
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bada_transcript <transcript.jsonl>...\n");
        return 2;
    }
    std::string error;
    const std::unique_ptr<EngineData> data = EngineData::Load(&error);
    if (!data) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    for (int i = 1; i < argc; ++i) {
        if (Run(argv[i], *data) == 2) return 2;
    }
    return test::Finish("transcripts");
}
