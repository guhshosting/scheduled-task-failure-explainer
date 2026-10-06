#include "model.hpp"
#include <stdexcept>

static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static bool contains(const stfe::Snapshot &s, const char *text) {
    return stfe::report(s).dump().find(text) != std::string::npos;
}
int main() {
    try {
        require(stfe::decode(0).kind == "success", "success");
        require(stfe::decode(0x80070002).failure, "missing file HRESULT");
        require(stfe::decode(0x80070005).failure, "access denied HRESULT");
        require(stfe::decode(0x41301).symbol == "SCHED_S_TASK_RUNNING", "running status");
        require(!stfe::decode(0x41303).failure, "never-run status");
        require(stfe::decode(0x80041320).symbol == "SCHED_E_USER_NOT_LOGGED_ON", "logon failure");
        require(stfe::decode(1).kind == "win32_or_application_exit", "application ambiguity");
        require(stfe::decode(0x80040154).kind == "hresult_or_application_exit",
                "unrelated HRESULT is not scheduler");
        require(!stfe::decode(0x41306).failure, "termination status not failed HRESULT");
        require(!stfe::decode(0xDEADBEEF).failure, "unknown high-bit code does not invent failure semantics");
        require(stfe::decode(0x80041325).kind != "task_scheduler",
                "undefined scheduler-range value not guessed");
        require(stfe::decode(0x80041322).symbol == "SCHED_E_SERVICE_NOT_AVAILABLE", "scheduler unavailable");
        stfe::Snapshot s;
        s.path = stfe::utf8(L"\\Folder\\工作😀");
        s.name = "test";
        s.last_run = "2026-09-30T01:00:00";
        s.enabled = false;
        s.result = 0x41303;
        s.battery_start = true;
        s.idle = true;
        s.limit = "PT1H";
        stfe::Action a;
        a.command = "C:\\missing.exe";
        a.probe = "missing";
        s.actions = {a, a};
        stfe::Trigger t;
        t.enabled = false;
        s.triggers = {t, t};
        for (const auto *text :
             {"disabled", "never_run", "power_condition", "idle_condition", "execution_limit",
              "missing_executable", "trigger_disabled", "history_unavailable"})
            require(contains(s, text), text);
        const auto j = stfe::report(s);
        const auto &o = std::get<stfe::Json::Object>(j.value);
        require(std::get<stfe::Json::Array>(o.at("actions").value).size() == 2, "multiple actions");
        require(std::get<stfe::Json::Array>(o.at("triggers").value).size() == 2, "multiple triggers");
        require(stfe::wide(s.path) == L"\\Folder\\工作😀", "unicode roundtrip");
        stfe::Event match;
        match.task = s.path;
        match.id = 201;
        match.data["result_code"] = "2";
        stfe::Event unrelated = match;
        unrelated.task += "Other";
        stfe::correlate(s, {unrelated, match});
        require(s.events.size() == 1, "exclude unrelated events");
        require(contains(s, "event_evidence"), "correlated events");
        const auto event_report = stfe::report(s);
        const auto &event_root = std::get<stfe::Json::Object>(event_report.value);
        const auto &history = std::get<stfe::Json::Object>(event_root.at("history").value);
        const auto &event_fields = std::get<stfe::Json::Object>(
            std::get<stfe::Json::Array>(history.at("events").value).front().value);
        require(std::holds_alternative<stfe::Json::Object>(event_fields.at("result_interpretation").value),
                "event code interpretation");
        stfe::Snapshot degradation;
        stfe::acquire_history(degradation, [](stfe::Snapshot &) {
            throw stfe::Failure(ERROR_ACCESS_DENIED, "synthetic history");
        });
        require(degradation.history_status == "access_denied" && !degradation.warnings.empty(),
                "history access denied degrades");
        stfe::acquire_history(degradation, [](stfe::Snapshot &) {
            throw stfe::Failure(ERROR_EVT_CHANNEL_NOT_FOUND, "synthetic history");
        });
        require(degradation.history_status == "unavailable", "history missing channel degrades");
        stfe::acquire_history(degradation, [](stfe::Snapshot &x) { x.history_status = "disabled"; });
        require(degradation.history_status == "disabled", "history disabled");
        require(contains(degradation, "history_channel_disabled"), "disabled channel limitation");
        require(stfe::history_query(L"\\O'Brien").find(L"=\"\\O'Brien\"") != std::wstring::npos,
                "XPath quote selection");
        require(stfe::history_query(L"\\a'\"b").find(L"TaskName") == std::wstring::npos,
                "XPath both quotes bounded fallback");
        require(stfe::history_query(L"\\task").find(L"604800000") != std::wstring::npos,
                "history time bound");
        stfe::acquire_history(degradation, [&](stfe::Snapshot &x) {
            x.path = s.path;
            stfe::correlate(x, {unrelated, match});
            x.history_status = "available";
        });
        require(degradation.history_status == "available" && degradation.events.size() == 1,
                "history available");
        require(!contains(degradation, "history_unavailable"), "available history no unavailable finding");
        stfe::Snapshot limit;
        limit.path = "\\a";
        stfe::Event many;
        many.task = "\\A";
        stfe::correlate(limit, std::vector<stfe::Event>(150, many));
        require(limit.events.size() == 100, "event bound and case insensitive identity");
        stfe::correlate(limit, {many});
        require(limit.events.size() == 100, "event bound across calls");
        a.command = "\\\\unreachable.invalid\\share\\tool.exe";
        a.probe = "not_applicable";
        stfe::probe_action(a);
        require(a.probe == "skipped_remote_or_device_path", "remote skipped");
        a.command = "C:\\stfe-definitely-nonexistent-3BB420BC.exe";
        stfe::probe_action(a);
        require(a.probe == "missing", "actual missing file");
        a.command = "cmd.exe";
        stfe::probe_action(a);
        require(a.probe == "skipped_ambiguous_path", "relative skipped");
        a.command = "C:\\folder\\..\\cmd.exe";
        stfe::probe_action(a);
        require(a.probe == "skipped_ambiguous_path", "dot path not followed");
        a.command = "\\\\?\\C:\\Windows\\System32\\cmd.exe";
        stfe::probe_action(a);
        require(a.probe == "skipped_remote_or_device_path", "device path skipped");
        require(stfe::action_type(5) == "com_handler" &&
                    stfe::action_type(6).find("deprecated") != std::string::npos,
                "other actions");
        s.history_status = "available";
        s.network = true;
        require(contains(s, "network_condition"), "network condition");
        s.limit = "PT0S";
        require(!contains(s, "\"id\":\"execution_limit\""), "unlimited not timeout");
        s.result = 0;
        s.last_run.clear();
        require(contains(s, "no_last_run") && !contains(s, "never_run"), "missing timestamp cautious");
        s.result = 0x80070005;
        require(contains(s, "last_result_failure"), "failure finding");
        require(stfe::parse({L"--json", L"Folder\\工作"}).path == L"\\Folder\\工作", "CLI Unicode nested");
        require(stfe::parse({L"--version"}).version, "CLI version");
        bool rejected = false;
        try {
            (void)stfe::parse({L"--json"});
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "CLI missing task");
        const auto escaped = stfe::Json(std::string("\"\\\n\t\x01")).dump();
        require(escaped == "\"\\\"\\\\\\n\\t\\u0001\"", "JSON escaping");
        // Independent parser consumes this fixture, not the serializer's own model.
        stfe::output(stfe::report(s).dump() + "\n");
        return 0;
    } catch (const std::exception &e) {
        try {
            stfe::output(std::string(e.what()) + "\n", true);
        } catch (...) {
            return 1;
        }
        return 1;
    } catch (...) {
        return 1;
    }
}
