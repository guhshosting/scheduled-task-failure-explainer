#ifndef STFE_MODEL_HPP
#define STFE_MODEL_HPP
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <windows.h>

namespace stfe {
// MSVC std::map moves may allocate their sentinel; inferred moves are intentionally
// allowed to throw and remain catchable. The tidy rule assumes moves never throw.
struct Json { // NOLINT(bugprone-exception-escape)
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;
    std::variant<std::nullptr_t, bool, int64_t, std::string, Array, Object> value;
    Json() : value(nullptr) {}
    // Implicit constructors allow type-safe JSON object initializer lists.
    // cppcheck-suppress noExplicitConstructor
    Json(bool v) : value(v) {}
    // cppcheck-suppress noExplicitConstructor
    Json(int v) : value(static_cast<int64_t>(v)) {}
    // cppcheck-suppress noExplicitConstructor
    Json(int64_t v) : value(v) {}
    // cppcheck-suppress noExplicitConstructor
    Json(const char *v) : value(std::string(v)) {}
    // cppcheck-suppress noExplicitConstructor
    Json(std::string v) : value(std::move(v)) {}
    // cppcheck-suppress noExplicitConstructor
    Json(Array v) : value(std::move(v)) {}
    // cppcheck-suppress noExplicitConstructor
    Json(Object v) : value(std::move(v)) {}
    Json &operator[](const std::string &key);
    std::string dump() const;
};
std::string utf8(const std::wstring &text);
std::wstring wide(const std::string &text);
std::string hex(uint32_t code);
std::string system_message(uint32_t code);
struct Result {
    uint32_t code = 0;
    std::string kind, symbol, message;
    bool failure = false;
};
Result decode(uint32_t code);
struct Action {
    int type = 0;
    std::string id, command, arguments, directory, class_id, data;
    std::string probe = "not_applicable";
};
struct Trigger { // NOLINT(bugprone-exception-escape): contains the throwing-move JSON map above.
    int type = 0;
    bool enabled = true;
    std::string id, start, end, execution_limit, repetition_interval, repetition_duration;
    Json details = Json::Object{};
};
struct Event { // NOLINT(bugprone-exception-escape): MSVC map move can allocate a sentinel.
    uint32_t id = 0;
    int64_t record = 0;
    std::string time, task, instance, message;
    std::map<std::string, std::string> data;
};
struct Snapshot { // NOLINT(bugprone-exception-escape): contains the throwing-move JSON map above.
    std::string path, name;
    bool enabled = true;
    int state = 0;
    uint32_t result = 0;
    std::string last_run, next_run;
    int missed = 0;
    std::string user, group;
    int logon = 0, run_level = 0;
    bool battery_start = false, battery_stop = false, idle = false, network = false;
    bool start_available = false, wake = false, demand = false, hard_terminate = false;
    bool idle_stop = false, idle_restart = false;
    int instances = 0, priority = 0;
    std::string limit, idle_duration, idle_wait, network_name, network_id;
    Json additional_settings = Json::Object{};
    std::vector<Action> actions;
    std::vector<Trigger> triggers;
    std::vector<Event> events;
    std::string history_status = "unavailable";
    std::vector<std::string> warnings;
};
struct Failure : std::exception {
    uint32_t code;
    std::string operation, message;
    Failure(uint32_t c, std::string op);
    const char *what() const noexcept override {
        return message.c_str();
    }
};
struct Arguments {
    bool json = false, help = false, version = false;
    std::wstring path;
};
Arguments parse(const std::vector<std::wstring> &args);
Snapshot acquire(const std::wstring &path);
void probe_action(Action &action);
void correlate(Snapshot &snapshot, const std::vector<Event> &candidates);
void acquire_history(Snapshot &snapshot, const std::function<void(Snapshot &)> &reader);
std::wstring history_query(const std::wstring &path);
Json report(const Snapshot &snapshot);
std::string human(const Snapshot &snapshot);
std::string action_type(int type);
std::string trigger_type(int type);
void output(const std::string &text, bool error = false);
} // namespace stfe
#endif
