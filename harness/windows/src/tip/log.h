// The diagnostic log, off by default. It records activations, focus and errors, never typed text.
// On with the harness setting "log": true: %LOCALAPPDATA%\bada\bada.log. %BADA_LOG_FILE% names
// another file and turns it on by itself (for tests).
#pragma once

namespace bada::tip::log {

void Enable(bool on);
void Write(const wchar_t* format, ...);

}  // namespace bada::tip::log
