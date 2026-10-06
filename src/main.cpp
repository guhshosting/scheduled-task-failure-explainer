#include "model.hpp"
#include <algorithm>
#include <stdexcept>

int wmain(int argc, wchar_t **argv) {
    bool json = false;
    try {
        std::vector<std::wstring> args;
        for (int i = 1; i < argc; ++i)
            args.emplace_back(argv[i]);
        json = std::find(args.begin(), args.end(), L"--json") != args.end();
        const auto parsed = stfe::parse(args);
        if (parsed.help) {
            stfe::output(
                "scheduled-task-failure-explainer 1.0.0\nUsage: scheduled-task-failure-explainer [--json] "
                "<task-path>\n       scheduled-task-failure-explainer --help | --version\nRead-only local "
                "Task Scheduler diagnostics. Nested folders and Unicode supported.\nNo elevation, task "
                "execution, edits, remote machines, or network probing.\nExit codes: 0 inspection succeeded "
                "(even when task failed); 2 usage; 3 acquisition; 4 internal/output.\n");
            return 0;
        }
        if (parsed.version) {
            stfe::output("scheduled-task-failure-explainer 1.0.0\n");
            return 0;
        }
        const auto snapshot = stfe::acquire(parsed.path);
        stfe::output(parsed.json ? stfe::report(snapshot).dump() + "\n" : stfe::human(snapshot));
        return 0;
    } catch (const std::exception &e) {
        const auto *failure = dynamic_cast<const stfe::Failure *>(&e);
        const int exit = dynamic_cast<const std::invalid_argument *>(&e) ? 2 : failure ? 3 : 4;
        try {
            if (json) {
                stfe::Json error = stfe::Json::Object{
                    {"schema_version", "1.0"},
                    {"application_version", "1.0.0"},
                    {"error", stfe::Json::Object{
                                  {"message", e.what()},
                                  {"exit_code", exit},
                                  {"code", failure ? stfe::Json(stfe::hex(failure->code)) : stfe::Json()}}}};
                stfe::output(error.dump() + "\n");
            } else
                stfe::output(std::string("Error: ") + e.what() + "\n", true);
        } catch (...) {
            return 4;
        }
        return exit;
    } catch (...) {
        try {
            if (json)
                stfe::output("{\"schema_version\":\"1.0\",\"application_version\":\"1.0.0\",\"error\":{"
                             "\"message\":\"Unexpected internal failure\",\"exit_code\":4,\"code\":null}}\n");
            else
                stfe::output("Error: unexpected internal failure\n", true);
        } catch (...) {
            return 4;
        }
        return 4;
    }
}
