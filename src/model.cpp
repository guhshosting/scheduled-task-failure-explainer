#include "model.hpp"
#include <algorithm>
#include <charconv>
#include <climits>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <taskschd.h>
#include <type_traits>

namespace stfe {
Json &Json::operator[](const std::string &key) {
    return std::get<Object>(value)[key];
}
static std::string quote(const std::string &s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (c < 32)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c)
                    << std::dec;
            else
                out << static_cast<char>(c);
        }
    }
    out << '"';
    return out.str();
}
std::string Json::dump() const {
    return std::visit(
        [](const auto &v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>)
                return "null";
            else if constexpr (std::is_same_v<T, bool>)
                return v ? "true" : "false";
            else if constexpr (std::is_same_v<T, int64_t>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, std::string>)
                return quote(v);
            else {
                std::string out = std::is_same_v<T, Array> ? "[" : "{";
                bool first = true;
                for (const auto &item : v) {
                    if (!first)
                        out += ',';
                    first = false;
                    if constexpr (std::is_same_v<T, Array>)
                        out += item.dump();
                    else
                        out += quote(item.first) + ':' + item.second.dump();
                }
                return out + (std::is_same_v<T, Array> ? "]" : "}");
            }
        },
        value);
}
std::string utf8(const std::wstring &text) {
    if (text.empty())
        return {};
    if (text.size() > INT_MAX)
        throw std::runtime_error("Unicode input too large");
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size)
        throw Failure(GetLastError(), "UTF-16 conversion");
    std::string result(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             result.data(), size, nullptr, nullptr))
        throw Failure(GetLastError(), "UTF-16 conversion");
    return result;
}
std::wstring wide(const std::string &text) {
    if (text.empty())
        return {};
    if (text.size() > INT_MAX)
        throw std::runtime_error("Unicode input too large");
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (!size)
        throw Failure(GetLastError(), "UTF-8 conversion");
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             result.data(), size))
        throw Failure(GetLastError(), "UTF-8 conversion");
    return result;
}
std::string hex(uint32_t code) {
    std::ostringstream s;
    s << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << code;
    return s.str();
}
std::string system_message(uint32_t code) {
    wchar_t buffer[4096]{};
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                             buffer, 4096, nullptr);
    if (!n) {
        // Already-loaded local module only: never search for or load a message DLL.
        const HMODULE module = GetModuleHandleW(L"taskschd.dll");
        if (module)
            n = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS, module, code, 0,
                               buffer, 4096, nullptr);
    }
    if (!n)
        return {};
    std::wstring s(buffer, n);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
        s.pop_back();
    return utf8(s);
}
Result decode(uint32_t code) {
    Result r;
    r.code = code;
    struct Known {
        uint32_t code;
        const char *symbol;
        const char *message;
    };
    static constexpr Known known[] = {
        {static_cast<uint32_t>(SCHED_S_TASK_READY), "SCHED_S_TASK_READY",
         "Task is ready to run at its next scheduled time."},
        {static_cast<uint32_t>(SCHED_S_TASK_RUNNING), "SCHED_S_TASK_RUNNING", "Task is currently running."},
        {static_cast<uint32_t>(SCHED_S_TASK_DISABLED), "SCHED_S_TASK_DISABLED", "Task is disabled."},
        {static_cast<uint32_t>(SCHED_S_TASK_HAS_NOT_RUN), "SCHED_S_TASK_HAS_NOT_RUN",
         "Task has not yet run."},
        {static_cast<uint32_t>(SCHED_S_TASK_NO_MORE_RUNS), "SCHED_S_TASK_NO_MORE_RUNS",
         "No more runs are scheduled."},
        {static_cast<uint32_t>(SCHED_S_TASK_NOT_SCHEDULED), "SCHED_S_TASK_NOT_SCHEDULED",
         "Required properties for scheduling are missing."},
        {static_cast<uint32_t>(SCHED_S_TASK_TERMINATED), "SCHED_S_TASK_TERMINATED",
         "The last run was terminated by the user."},
        {static_cast<uint32_t>(SCHED_S_TASK_NO_VALID_TRIGGERS), "SCHED_S_TASK_NO_VALID_TRIGGERS",
         "Task has no valid enabled triggers."},
        {static_cast<uint32_t>(SCHED_S_EVENT_TRIGGER), "SCHED_S_EVENT_TRIGGER",
         "Event triggers do not have scheduled run times."},
        {static_cast<uint32_t>(SCHED_S_SOME_TRIGGERS_FAILED), "SCHED_S_SOME_TRIGGERS_FAILED",
         "Some triggers failed to register."},
        {static_cast<uint32_t>(SCHED_S_BATCH_LOGON_PROBLEM), "SCHED_S_BATCH_LOGON_PROBLEM",
         "Batch logon privilege may be missing."},
        {static_cast<uint32_t>(SCHED_S_TASK_QUEUED), "SCHED_S_TASK_QUEUED", "Task is queued."},
        {static_cast<uint32_t>(SCHED_E_TRIGGER_NOT_FOUND), "SCHED_E_TRIGGER_NOT_FOUND",
         "The trigger was not found."},
        {static_cast<uint32_t>(SCHED_E_TASK_NOT_READY), "SCHED_E_TASK_NOT_READY",
         "Properties required to run the task have not been set."},
        {static_cast<uint32_t>(SCHED_E_TASK_NOT_RUNNING), "SCHED_E_TASK_NOT_RUNNING",
         "No instance of the task is running."},
        {static_cast<uint32_t>(SCHED_E_SERVICE_NOT_INSTALLED), "SCHED_E_SERVICE_NOT_INSTALLED",
         "Task Scheduler service is not installed."},
        {static_cast<uint32_t>(SCHED_E_CANNOT_OPEN_TASK), "SCHED_E_CANNOT_OPEN_TASK",
         "The task object could not be opened."},
        {static_cast<uint32_t>(SCHED_E_INVALID_TASK), "SCHED_E_INVALID_TASK",
         "The object is not a valid task."},
        {static_cast<uint32_t>(SCHED_E_ACCOUNT_DBASE_CORRUPT), "SCHED_E_ACCOUNT_DBASE_CORRUPT",
         "Task Scheduler reported corruption in its security database."},
        {static_cast<uint32_t>(SCHED_E_NO_SECURITY_SERVICES), "SCHED_E_NO_SECURITY_SERVICES",
         "Legacy status: Task Scheduler security services require Windows NT."},
        {static_cast<uint32_t>(SCHED_E_UNKNOWN_OBJECT_VERSION), "SCHED_E_UNKNOWN_OBJECT_VERSION",
         "The task object version is invalid or unsupported."},
        {static_cast<uint32_t>(SCHED_E_UNSUPPORTED_ACCOUNT_OPTION), "SCHED_E_UNSUPPORTED_ACCOUNT_OPTION",
         "The account settings and runtime options use an unsupported combination."},
        {static_cast<uint32_t>(SCHED_E_UNEXPECTEDNODE), "SCHED_E_UNEXPECTEDNODE",
         "The task XML has an unexpected node."},
        {static_cast<uint32_t>(SCHED_E_NAMESPACE), "SCHED_E_NAMESPACE",
         "The task XML uses an unexpected namespace."},
        {static_cast<uint32_t>(SCHED_E_INVALIDVALUE), "SCHED_E_INVALIDVALUE",
         "The task XML has an incorrectly formatted or out-of-range value."},
        {static_cast<uint32_t>(SCHED_E_MISSINGNODE), "SCHED_E_MISSINGNODE",
         "A required task XML element or attribute is missing."},
        {static_cast<uint32_t>(SCHED_E_MALFORMEDXML), "SCHED_E_MALFORMEDXML", "The task XML is malformed."},
        {static_cast<uint32_t>(SCHED_E_TOO_MANY_NODES), "SCHED_E_TOO_MANY_NODES",
         "The task XML has too many nodes of one type."},
        {static_cast<uint32_t>(SCHED_E_PAST_END_BOUNDARY), "SCHED_E_PAST_END_BOUNDARY",
         "The task cannot start after its trigger's end boundary."},
        {static_cast<uint32_t>(SCHED_E_INVALID_TASK_HASH), "SCHED_E_INVALID_TASK_HASH",
         "Task Scheduler reported a corrupt or altered task image."},
        {static_cast<uint32_t>(SCHED_E_SERVICE_NOT_AVAILABLE), "SCHED_E_SERVICE_NOT_AVAILABLE",
         "Task Scheduler service is unavailable."},
        {static_cast<uint32_t>(SCHED_E_SERVICE_TOO_BUSY), "SCHED_E_SERVICE_TOO_BUSY",
         "Task Scheduler service is too busy to handle the request."},
        {static_cast<uint32_t>(SCHED_E_TASK_NOT_V1_COMPAT), "SCHED_E_TASK_NOT_V1_COMPAT",
         "Task properties are incompatible with earlier Windows versions."},
        {static_cast<uint32_t>(SCHED_E_TASK_NOT_UBPM_COMPAT), "SCHED_E_TASK_NOT_UBPM_COMPAT",
         "Task properties are incompatible with the scheduling engine."},
        {static_cast<uint32_t>(SCHED_E_DEPRECATED_FEATURE_USED), "SCHED_E_DEPRECATED_FEATURE_USED",
         "The task definition uses a deprecated feature."},
        {static_cast<uint32_t>(SCHED_E_ACCOUNT_INFORMATION_NOT_SET), "SCHED_E_ACCOUNT_INFORMATION_NOT_SET",
         "Account information has not been set."},
        {static_cast<uint32_t>(SCHED_E_ACCOUNT_NAME_NOT_FOUND), "SCHED_E_ACCOUNT_NAME_NOT_FOUND",
         "The account name was not found."},
        {static_cast<uint32_t>(SCHED_E_SERVICE_NOT_RUNNING), "SCHED_E_SERVICE_NOT_RUNNING",
         "Task Scheduler service is not running."},
        {static_cast<uint32_t>(SCHED_E_ALREADY_RUNNING), "SCHED_E_ALREADY_RUNNING",
         "An instance of this task is already running."},
        {static_cast<uint32_t>(SCHED_E_USER_NOT_LOGGED_ON), "SCHED_E_USER_NOT_LOGGED_ON",
         "The user is not logged on."},
        {static_cast<uint32_t>(SCHED_E_TASK_ATTEMPTED), "SCHED_E_TASK_ATTEMPTED",
         "Task was attempted but did not run due to a constraint."},
        {static_cast<uint32_t>(SCHED_E_TASK_DISABLED), "SCHED_E_TASK_DISABLED", "Task is disabled."},
        {static_cast<uint32_t>(SCHED_E_START_ON_DEMAND), "SCHED_E_START_ON_DEMAND",
         "Task settings do not allow on-demand starts."}};
    if (code == 0) {
        r.kind = "success";
        r.message = "Last result is zero (success).";
        return r;
    }
    const auto found =
        std::find_if(std::begin(known), std::end(known), [code](const Known &k) { return k.code == code; });
    if (found != std::end(known)) {
        r.kind = "task_scheduler";
        r.symbol = found->symbol;
        r.message = found->message;
        r.failure = (code & 0x80000000U) != 0;
        return r;
    }
    if ((code & 0xFFFF0000U) == 0x80070000U) {
        r.kind = "hresult_win32";
        r.failure = true;
        r.message = system_message(code & 0xFFFFU);
    } else if ((code & 0x80000000U) != 0) {
        r.kind = "hresult_or_application_exit";
        r.message = system_message(code);
        r.failure = !r.message.empty();
    } else {
        r.kind = "win32_or_application_exit";
        r.message = system_message(code);
        // An application exit code has no universal meaning; a Win32 match is a candidate only.
    }
    if (r.message.empty())
        r.message = "No local system message is available; application-specific exit codes require the "
                    "application's documentation.";
    if (r.kind == "win32_or_application_exit" || r.kind == "hresult_or_application_exit")
        r.message += " This numeric value may instead be an application-defined exit code; the numeric match "
                     "does not establish its origin.";
    return r;
}
Failure::Failure(uint32_t c, std::string op)
    : code(c), operation(std::move(op)), message(operation + ": " + hex(c) + " " + system_message(c)) {}
Arguments parse(const std::vector<std::wstring> &args) {
    Arguments a;
    for (const auto &arg : args) {
        if (arg == L"--json" && !a.json)
            a.json = true;
        else if (arg == L"--help" && !a.help)
            a.help = true;
        else if (arg == L"--version" && !a.version)
            a.version = true;
        else if (arg.empty() || arg.front() == L'-' || !a.path.empty())
            throw std::invalid_argument("Expected one task path and optional --json, or --help / --version");
        else
            a.path = arg;
    }
    if (a.help || a.version) {
        if (args.size() != 1)
            throw std::invalid_argument("--help and --version must be used alone");
    } else {
        if (a.path.empty())
            throw std::invalid_argument("A task path is required; use --help");
        if (a.path.front() != L'\\')
            a.path.insert(a.path.begin(), L'\\');
        if (a.path.back() == L'\\' || a.path.find(L"\\\\") != std::wstring::npos ||
            a.path.find(L'/') != std::wstring::npos)
            throw std::invalid_argument("Invalid Task Scheduler path");
    }
    return a;
}
std::string action_type(int t) {
    switch (t) {
    case 0:
        return "execute";
    case 5:
        return "com_handler";
    case 6:
        return "send_email_legacy_deprecated";
    case 7:
        return "show_message_legacy_deprecated";
    default:
        return "unknown(" + std::to_string(t) + ")";
    }
}
std::string trigger_type(int t) {
    static const char *names[] = {
        "event", "time",         "daily", "weekly", "monthly", "monthly_day_of_week",
        "idle",  "registration", "boot",  "logon",  "unused",  "session_state_change",
        "custom"};
    return t >= 0 && t < 13 ? names[t] : "unknown(" + std::to_string(t) + ")";
}
static std::string enum_name(int n, const std::vector<std::string> &values) {
    return n >= 0 && static_cast<size_t>(n) < values.size() ? values[static_cast<size_t>(n)]
                                                            : "unknown(" + std::to_string(n) + ")";
}
void correlate(Snapshot &s, const std::vector<Event> &candidates) {
    for (const auto &e : candidates) {
        if (s.events.size() >= 100)
            break;
        // Task path comparison is case-insensitive ordinal, like Task Scheduler identity.
        const auto a = wide(s.path), b = wide(e.task);
        if (CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL) {
            s.events.push_back(e);
        }
    }
}
void acquire_history(Snapshot &s, const std::function<void(Snapshot &)> &reader) {
    try {
        reader(s);
    } catch (const Failure &e) {
        s.history_status = e.code == ERROR_ACCESS_DENIED ? "access_denied" : "unavailable";
        s.warnings.push_back(e.message);
    }
}
std::wstring history_query(const std::wstring &path) {
    std::wstring query =
        L"*[System[TimeCreated[timediff(@SystemTime) >= 0 and timediff(@SystemTime) <= 604800000]]]";
    if (path.find(L'\'') == std::wstring::npos)
        query += L" and *[EventData[Data[@Name='TaskName']='" + path + L"']]";
    else if (path.find(L'"') == std::wstring::npos)
        query += L" and *[EventData[Data[@Name='TaskName']=\"" + path + L"\"]]";
    return query;
}
static Json event_result(const std::map<std::string, std::string> &data) {
    auto it = data.find("result_code");
    if (it == data.end())
        it = data.find("error_code");
    if (it == data.end())
        return {};
    const auto &text = it->second;
    uint32_t code = 0;
    const bool hexadecimal = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    const char *begin = text.data() + (hexadecimal ? 2 : 0);
    const auto parsed = std::from_chars(begin, text.data() + text.size(), code, hexadecimal ? 16 : 10);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
        return {};
    const auto r = decode(code);
    return Json::Object{{"hex", hex(code)},
                        {"kind", r.kind},
                        {"symbol", r.symbol},
                        {"message", r.message},
                        {"known_failure", r.failure}};
}
Json report(const Snapshot &s) {
    Json root = Json::Object{};
    root["schema_version"] = "1.0";
    root["application_version"] = "1.0.0";
    root["task"] = Json::Object{{"path", s.path}, {"name", s.name}};
    root["runtime"] =
        Json::Object{{"enabled", s.enabled},
                     {"state", enum_name(s.state, {"unknown", "disabled", "queued", "ready", "running"})},
                     {"state_value", s.state},
                     {"last_run_time_local", s.last_run.empty() ? Json() : Json(s.last_run)},
                     {"next_run_time_local", s.next_run.empty() ? Json() : Json(s.next_run)},
                     {"missed_runs", s.missed}};
    root["configuration"] = Json::Object{
        {"principal",
         Json::Object{
             {"user_id", s.user},
             {"group_id", s.group},
             {"logon_type", enum_name(s.logon, {"none", "password", "s4u", "interactive_token", "group",
                                                "service_account", "interactive_token_or_password"})},
             {"logon_type_value", s.logon},
             {"run_level", enum_name(s.run_level, {"least_privilege", "highest_available"})},
             {"run_level_value", s.run_level}}},
        {"settings",
         Json::Object{{"execution_time_limit", s.limit},
                      {"multiple_instances",
                       enum_name(s.instances, {"parallel", "queue", "ignore_new", "stop_existing"})},
                      {"multiple_instances_value", s.instances},
                      {"start_when_available", s.start_available},
                      {"wake_to_run", s.wake},
                      {"allow_demand_start", s.demand},
                      {"allow_hard_terminate", s.hard_terminate},
                      {"priority", s.priority},
                      {"disallow_start_on_batteries", s.battery_start},
                      {"stop_if_going_on_batteries", s.battery_stop},
                      {"run_only_if_idle", s.idle},
                      {"idle_duration", s.idle_duration},
                      {"idle_wait_timeout", s.idle_wait},
                      {"stop_on_idle_end", s.idle_stop},
                      {"restart_on_idle", s.idle_restart},
                      {"run_only_if_network_available", s.network},
                      {"network_profile_name", s.network_name},
                      {"network_profile_id", s.network_id}}}};
    auto &config = std::get<Json::Object>(root["configuration"].value);
    auto &settings = std::get<Json::Object>(config.at("settings").value);
    for (const auto &field : std::get<Json::Object>(s.additional_settings.value))
        settings.emplace(field);
    Json::Array actions, triggers, events, findings, warnings;
    std::transform(s.actions.begin(), s.actions.end(), std::back_inserter(actions), [](const Action &a) {
        return Json::Object{
            {"type", action_type(a.type)}, {"type_value", a.type},     {"id", a.id},
            {"command", a.command},        {"arguments", a.arguments}, {"working_directory", a.directory},
            {"class_id", a.class_id},      {"data", a.data},           {"local_path_check", a.probe}};
    });
    std::transform(s.triggers.begin(), s.triggers.end(), std::back_inserter(triggers), [](const Trigger &t) {
        return Json::Object{{"type", trigger_type(t.type)},
                            {"type_value", t.type},
                            {"id", t.id},
                            {"enabled", t.enabled},
                            {"start_boundary", t.start},
                            {"end_boundary", t.end},
                            {"execution_time_limit", t.execution_limit},
                            {"repetition_interval", t.repetition_interval},
                            {"repetition_duration", t.repetition_duration},
                            {"details", t.details}};
    });
    for (const auto &e : s.events) {
        Json::Object data;
        for (const auto &p : e.data)
            data.emplace(p.first, p.second);
        events.emplace_back(Json::Object{{"id", static_cast<int64_t>(e.id)},
                                         {"record_id", e.record},
                                         {"time_utc", e.time},
                                         {"task_path", e.task},
                                         {"instance_id", e.instance},
                                         {"message", e.message},
                                         {"data", data},
                                         {"result_interpretation", event_result(e.data)}});
    }
    root["actions"] = actions;
    root["triggers"] = triggers;
    root["history"] = Json::Object{{"status", s.history_status},
                                   {"window_days", 7},
                                   {"max_examined_events", 512},
                                   {"max_returned_events", 100},
                                   {"events", events}};
    const auto r = decode(s.result);
    root["last_result"] =
        Json::Object{{"unsigned_decimal", static_cast<int64_t>(s.result)},
                     {"hex", hex(s.result)},
                     {"kind", r.kind},
                     {"symbol", r.symbol},
                     {"message", r.message},
                     {"known_failure", r.failure},
                     {"win32_error_code",
                      r.kind == "hresult_win32" ? Json(static_cast<int64_t>(s.result & 0xFFFFU)) : Json()}};
    const auto finding = [&](const char *id, const char *basis, const std::string &message) {
        findings.emplace_back(Json::Object{{"id", id}, {"basis", basis}, {"message", message}});
    };
    if (r.failure)
        finding("last_result_failure", "runtime", "Last result matches a failure code: " + r.message);
    else if (s.result != 0 && r.kind != "task_scheduler")
        finding("nonzero_result", "runtime",
                "Last result is nonzero; application exit-code semantics are not known.");
    if (!s.enabled)
        finding("disabled", "runtime", "Task is disabled.");
    if (s.result == 0x41303)
        finding("never_run", "runtime", "Task Scheduler reports that this task has never run.");
    else if (s.last_run.empty())
        finding("no_last_run", "runtime",
                "No usable last-run timestamp is available; this alone does not prove the task never ran.");
    if (s.missed > 0)
        finding("missed_runs", "runtime", "Task Scheduler reports missed runs: " + std::to_string(s.missed));
    for (const auto &t : s.triggers)
        if (!t.enabled)
            finding("trigger_disabled", "configuration",
                    "Possible contributing condition: trigger '" + t.id + "' is disabled.");
    const auto &extra = std::get<Json::Object>(s.additional_settings.value);
    const auto maintenance = extra.find("maintenance");
    const bool has_maintenance =
        maintenance != extra.end() && std::holds_alternative<Json::Object>(maintenance->second.value);
    if (has_maintenance)
        finding("maintenance_scheduling", "configuration",
                "Automatic maintenance settings are configured; run opportunities also depend on Windows "
                "maintenance scheduling.");
    if (s.triggers.empty() && !has_maintenance)
        finding("no_triggers", "configuration",
                "No triggers are configured; on-demand execution may still be possible.");
    if (s.battery_start || s.battery_stop)
        finding(
            "power_condition", "configuration",
            "Possible contributing condition: battery settings restrict starting or continued execution.");
    if (s.idle)
        finding("idle_condition", "configuration",
                "Possible contributing condition: execution requires the machine to be idle.");
    if (s.network)
        finding("network_condition", "configuration",
                "Possible contributing condition: a network availability condition is configured. Network "
                "state was not tested.");
    if (!s.limit.empty() && s.limit != "PT0S")
        finding("execution_limit", "configuration",
                "Possible contributing condition: execution time limit is " + s.limit +
                    "; this does not prove a timeout occurred.");
    if (s.logon == 3 || s.logon == 6)
        finding("interactive_logon", "configuration",
                "Possible contributing condition: the principal uses an interactive logon mode.");
    for (const auto &a : s.actions) {
        if (a.probe == "missing")
            finding("missing_executable", "local_filesystem",
                    "Configured local action path was not found in this process's environment at inspection "
                    "time: " +
                        a.command);
        if (a.probe == "is_directory")
            finding("executable_is_directory", "local_filesystem",
                    "Configured action path resolves to a local directory at inspection time: " + a.command);
    }
    if (!s.events.empty())
        finding("event_evidence", "event_log",
                "Recent events reference this task path. They may cover multiple executions; inspect "
                "timestamps and instance IDs before attributing them to the last result.");
    if (s.history_status == "disabled")
        finding("history_channel_disabled", "limitation",
                "Operational channel is disabled. Retained events, if any, may be shown; new executions may "
                "not be recorded.");
    else if (s.history_status != "available")
        finding("history_unavailable", "limitation",
                "Event history is " + s.history_status +
                    "; task configuration and runtime inspection remain usable.");
    std::copy(s.warnings.begin(), s.warnings.end(), std::back_inserter(warnings));
    warnings.emplace_back(
        "Snapshot is not atomic; task configuration and runtime state can change during inspection.");
    warnings.emplace_back("LastTaskResult can be an application exit code. Numeric message matches do not "
                          "establish causality.");
    warnings.emplace_back("API run times are local wall times without a timezone offset; absent next-run "
                          "time is normal for some trigger types.");
    warnings.emplace_back("Action environment variables expand in the diagnostic process context, which may "
                          "differ from the task principal's environment.");
    warnings.emplace_back("History is limited to seven days, 512 examined events, and 100 matching events; "
                          "absence of evidence does not prove no execution.");
    root["findings"] = findings;
    root["warnings"] = warnings;
    return root;
}
std::string human(const Snapshot &s) {
    const auto r = report(s);
    const auto &o = std::get<Json::Object>(r.value);
    std::ostringstream out;
    const auto yes = [](bool b) { return b ? "yes" : "no"; };
    out << "Task: " << quote(s.path) << "\nEnabled: " << yes(s.enabled)
        << "\nState: " << enum_name(s.state, {"unknown", "disabled", "queued", "ready", "running"})
        << "\nLast run (local): " << (s.last_run.empty() ? "unavailable" : s.last_run)
        << "\nNext run (local): " << (s.next_run.empty() ? "unavailable" : s.next_run)
        << "\nMissed runs: " << s.missed << "\nLast result: " << hex(s.result) << " - "
        << decode(s.result).message << "\nPrincipal user: " << quote(s.user)
        << "\nPrincipal group: " << quote(s.group) << "\nLogon mode: "
        << enum_name(s.logon, {"none", "password", "s4u", "interactive_token", "group", "service_account",
                               "interactive_token_or_password"})
        << "\nRun level: " << enum_name(s.run_level, {"least_privilege", "highest_available"})
        << "\nExecution limit: " << quote(s.limit) << "\nMultiple instances: "
        << enum_name(s.instances, {"parallel", "queue", "ignore_new", "stop_existing"})
        << "\nStart when available: " << yes(s.start_available) << "; wake to run: " << yes(s.wake)
        << "; demand start: " << yes(s.demand) << "; hard terminate: " << yes(s.hard_terminate)
        << "; priority: " << s.priority << "\nPower: disallow battery start=" << yes(s.battery_start)
        << "; stop on batteries=" << yes(s.battery_stop) << "\nIdle: required=" << yes(s.idle)
        << "; duration=" << quote(s.idle_duration) << "; wait=" << quote(s.idle_wait)
        << "; stop on idle end=" << yes(s.idle_stop) << "; restart=" << yes(s.idle_restart)
        << "\nNetwork condition: required=" << yes(s.network) << "; profile=" << quote(s.network_name)
        << "; ID=" << quote(s.network_id) << "\n";
    for (const auto &field : std::get<Json::Object>(s.additional_settings.value))
        out << "Setting " << field.first << ": " << field.second.dump() << "\n";
    for (const auto &a : s.actions)
        out << "Action [" << action_type(a.type) << "]: " << quote(a.command)
            << "\n  Arguments: " << quote(a.arguments) << "\n  Working directory: " << quote(a.directory)
            << "\n  Local path check: " << a.probe << "\n  Class ID: " << quote(a.class_id)
            << "\n  Data: " << quote(a.data) << "\n";
    for (const auto &t : s.triggers)
        out << "Trigger [" << trigger_type(t.type) << "] " << quote(t.id) << ": "
            << (t.enabled ? "enabled" : "disabled") << ", start=" << quote(t.start)
            << ", end=" << quote(t.end) << ", limit=" << quote(t.execution_limit)
            << ", repetition=" << quote(t.repetition_interval) << "/" << quote(t.repetition_duration)
            << ", details=" << t.details.dump() << "\n";
    out << "Event history: " << s.history_status << "\n";
    for (const auto &e : s.events) {
        out << "  " << e.time << " event=" << e.id << " record=" << e.record << " instance=" << e.instance;
        for (const auto &item : e.data)
            out << " " << item.first << "=" << quote(item.second);
        out << "\n";
        if (!e.message.empty())
            out << "    " << quote(e.message) << "\n";
    }
    for (const auto &f : std::get<Json::Array>(o.at("findings").value)) {
        const auto &fields = std::get<Json::Object>(f.value);
        out << "Finding [" << std::get<std::string>(fields.at("basis").value)
            << "]: " << quote(std::get<std::string>(fields.at("message").value)) << "\n";
    }
    for (const auto &w : std::get<Json::Array>(o.at("warnings").value))
        out << "Limitation: " << std::get<std::string>(w.value) << "\n";
    return out.str();
}
void output(const std::string &text, bool error) {
    HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        const auto s = wide(text);
        DWORD written = 0;
        if (!WriteConsoleW(handle, s.data(), static_cast<DWORD>(s.size()), &written, nullptr) ||
            written != s.size())
            throw Failure(GetLastError(), "console output");
    } else {
        size_t offset = 0;
        while (offset < text.size()) {
            DWORD written = 0;
            const DWORD count = static_cast<DWORD>(std::min<size_t>(text.size() - offset, 65536));
            if (!WriteFile(handle, text.data() + offset, count, &written, nullptr) || !written)
                throw Failure(GetLastError(), "output");
            offset += written;
        }
    }
}
} // namespace stfe
