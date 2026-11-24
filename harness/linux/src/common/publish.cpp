#include "publish.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dbus/dbus.h>

namespace bada {
namespace fs = std::filesystem;

namespace {

// Writes `text` to `path` through a temporary file in the same folder, then a rename: a reader
// sees the old file or the new one, never half of one. Mode 0600: it holds what is being typed.
bool WriteAtomically(const fs::path& path, const std::string& text) {
    const fs::path tmp = path.string() + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n <= 0) {
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        done += static_cast<size_t>(n);
    }
    ::close(fd);
    return ::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

fs::path Publisher::StatePath() {
    if (const char* run = std::getenv("XDG_RUNTIME_DIR"); run && *run == '/') {
        return fs::path(run) / "bada" / "state.json";
    }
    return fs::path("/tmp") / ("bada-" + std::to_string(::getuid())) / "state.json";
}

Publisher::Publisher() : path_(StatePath()) {}

Publisher::~Publisher() {
    if (bus_) {
        auto* c = static_cast<DBusConnection*>(bus_);
        dbus_connection_close(c);
        dbus_connection_unref(c);
    }
}

void Publisher::Publish(const std::string& json) {
    if (json == last_) return;
    last_ = json;
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    ::chmod(path_.parent_path().c_str(), 0700);  // only the user reads it
    WriteAtomically(path_, json + "\n");
    Signal(json);
}

void Publisher::Clear() {
    if (last_ == "{}") return;
    last_ = "{}";
    std::error_code ec;
    fs::remove(path_, ec);
    Signal("{}");
}

void Publisher::Signal(const std::string& json) {
    if (!bus_tried_) {
        bus_tried_ = true;
        DBusError error;
        dbus_error_init(&error);
        // A private connection: the framework's own stays untouched.
        DBusConnection* c = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
        if (c) {
            dbus_connection_set_exit_on_disconnect(c, FALSE);
            bus_ = c;
        }
        dbus_error_free(&error);
    }
    if (!bus_) return;
    auto* c = static_cast<DBusConnection*>(bus_);
    if (!dbus_connection_get_is_connected(c)) return;
    // Nothing is ever read from this connection: drop what the bus sent (NameAcquired).
    dbus_connection_read_write(c, 0);
    while (DBusMessage* in = dbus_connection_pop_message(c)) dbus_message_unref(in);
    DBusMessage* m = dbus_message_new_signal(kStateObject, kStateInterface, "Changed");
    if (!m) return;
    const char* text = json.c_str();
    if (dbus_validate_utf8(text, nullptr) && dbus_message_append_args(m, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID)) {
        dbus_connection_send(c, m, nullptr);
        dbus_connection_flush(c);
    }
    dbus_message_unref(m);
}

}  // namespace bada
