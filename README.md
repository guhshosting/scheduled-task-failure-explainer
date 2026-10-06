# scheduled-task-failure-explainer

> Release context: see [qualified platforms, runtime requirements and package scope](RELEASE-QUALIFICATION.md).


Version **1.0.0**. A native Unicode Windows CLI that consolidates a local scheduled
task's configuration, runtime state, last result, and recent Operational events.
It diagnoses; it does not repair or execute tasks.

```text
scheduled-task-failure-explainer "\Microsoft\Windows\Defrag\ScheduledDefrag"
scheduled-task-failure-explainer --json "\My Folder\Daily Report"
scheduled-task-failure-explainer --help
scheduled-task-failure-explainer --version
```

Task paths use backslashes and support nested folders and Unicode. An initial
backslash is optional. Quote paths containing spaces. JSON and redirected text
are UTF-8 without a BOM; interactive consoles receive UTF-16 through WriteConsoleW.

The report includes actions (commands, arguments, directories or COM handler IDs),
principal user/group, logon mode, run level, enabled/current state, local last/next
run times, missed runs, triggers, repetition and schedule details, execution limit,
multiple-instance policy, idle, power and network conditions. Schedule masks are
the Windows SDK bitmasks (days Sunday through Saturday; months January through
December). Legacy email/message actions are explicitly marked deprecated; their
summary is intentionally limited. No action or COM handler is instantiated or run.
Restart policy, RemoteApp session restrictions, unified scheduling engine usage,
and automatic maintenance period/deadline/exclusivity are also reported when exposed.
Maintenance tasks can legitimately have no ordinary triggers.

## Evidence and interpretation

Runtime values come from Task Scheduler 2.0 COM getters. Configuration restrictions
are described as **possible contributing conditions**, never as established causes.
An idle requirement, battery restriction, or time limit does not prove it blocked
the last execution. A missing local file is observed at inspection time, not at the
time of the last run. Configuration and runtime acquisition is not atomic.

LastTaskResult is represented as unsigned decimal and eight-digit hexadecimal.
Zero denotes success. Known Task Scheduler statuses and errors use SDK definitions;
HRESULTs wrapping Win32 errors use the local Windows message. Unknown values are
not assigned invented meanings. A program's exit code can coincide with a Win32
error or HRESULT: matching a numeric message does not establish the code's origin
or prove a particular failure cause. A nonzero Task Scheduler success/status code
(for example running or never run) is not treated as a failed HRESULT. Consult the
action's documentation for application-specific results.

Operational history uses Windows Event Log APIs locally. Queries cover seven days,
newest first, and normally filter TaskName at the server. A bounded fallback for
paths containing both quote styles applies exact, ordinal case-insensitive matching
on the client. The reader examines at most 512 events, returns at most 100 matching
events, and checks a three-second deadline between reads. Individual EvtNext calls
have a 100-ms timeout; metadata/query API calls do not offer an overall timeout.
Structured event fields include IDs, UTC times, record IDs, instance IDs, result/
error codes, action names and user context; local publisher messages are included
when available. Result fields receive the same cautious decoder as LastTaskResult.
Events are correlated by task path; **they are not assumed to belong to the last
execution**. Compare timestamps and instance IDs. Events can cover older executions
or an earlier task with the same path. Absence of events proves nothing about runs.

The tool never enables the Operational channel. It can still query retained events
when the channel is disabled, and labels that state explicitly. Disabled logs, permissions,
retention, missing message resources, query timeouts, and bounded results can limit
history. History failure degrades the report without discarding successful task
inspection. No last-run timestamp alone is not conclusive evidence of never running;
the explicit SCHED_S_TASK_HAS_NOT_RUN status is reported separately. Next-run time
may be absent for event/logon triggers. COM dates are local wall times with no offset.

## Safety and permissions

The application is strictly read-only. It uses Task Scheduler getters, local
Connect/GetFolder/GetTask, and Event Log read APIs. It has no task run/stop/register/
delete/set operations, credential retrieval, log clearing, or channel writes.
COM objects, BSTRs, apartment lifetime, event handles, and file handles use RAII.

There is no intentional networking: no remote service/session, hostname resolution,
network-condition test, remote action probe, or child process. File checks apply only
to unambiguous absolute paths on directly mapped local fixed volumes. Environment
variables expand in the diagnostic process's context, which may differ from the
task principal's environment. Relative paths, device/UNC paths, mapped/nonlocal
drives, and reparse points are skipped. Ancestor handles remain open without write/
delete sharing during local read-only path inspection. A probe is an existence
observation, not executable validation. Local COM uses the OS scheduler service;
the tool does not configure or test the service's own behavior.

No automatic elevation occurs. Run with your normal account first. Task and history
ACLs apply; if your account lacks access, the tool reports access denied. Running it
manually under a different identity may expose more data. Reports can contain task
arguments and configured handler data; share them with appropriate care.

## JSON and exit codes

`--json` emits exactly one JSON document on stdout, including structured errors;
diagnostic chatter is excluded. Schema version is `1.0`, application version is
`1.0.0`. Top-level fields: `task`, `runtime`, `configuration`, `actions`, `triggers`,
`last_result`, `history`, `findings`, `warnings`. Absent run times and undecodable
event result fields are null. Empty configured strings remain strings. An unavailable
missed-run count is -1 with a warning. Unknown enum values retain their numeric value.
Consumers should tolerate additive fields and use `schema_version` for compatibility.
See [docs/schema.json](docs/schema.json).

| Exit | Meaning |
|---|---|
| 0 | Inspection succeeded, even if the task result indicates failure or history is unavailable |
| 2 | Invalid command line |
| 3 | Acquisition failed (COM, missing task/folder, access denied, unavailable scheduler, malformed data) |
| 4 | Unexpected internal or output failure |

## Compatibility and build

Target: Windows 10, Windows 11, and corresponding supported Windows Server releases,
x64. Windows 8.1 and older are outside scope. **Only Windows Server 2025 x64 was
runtime-tested in this workspace.** See [VALIDATION.md](VALIDATION.md) for exact scope.

Requires CMake 3.24+, MSVC C++17, Windows SDK, and Ninja or a Visual Studio generator.
There are no downloaded dependencies. Production builds statically link the MSVC
runtime; the installed Release executable needs only Windows system DLLs. ASan uses
the dynamic runtime and copies its instrumentation DLL into the test build directory.
In an x64 Visual Studio developer terminal:

```bat
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
cmake --install build-release --prefix install
```

`tools\dev.cmd <command>` locates the installed MSVC developer environment.
`/W4 /WX /utf-8 /permissive-` apply to project sources. Use `-DSTFE_ANALYZE=ON`
for `/analyze`; use `-DSTFE_ASAN=ON` with Release or RelWithDebInfo (Debug's `/RTC1`
is incompatible with ASan). The ASan runtime must be on PATH, as in the developer
terminal. For clang-tidy, export a Ninja compilation database and run
`tools\dev.cmd "C:\Program Files\LLVM\bin\clang-tidy.exe" -p build-release src\model.cpp`
(also acquire.cpp, main.cpp and tests.cpp). Cppcheck can use its Windows platform
and MSVC library configuration. See `tools/validate.cmd` for repeatable checks.
When an execution sandbox prevents ASan startup, `STFE_SKIP_ASAN_RUNTIME=1` skips
only that runtime step in the script; run the ASan CTest suite separately in an
authorized compatible environment and record the result explicitly.

Synthetic tests separate acquisition from analysis and do not create or modify tasks.
Integration tests are opt-in: `-DSTFE_INTEGRATION=ON`, optionally
`-DSTFE_TEST_TASK="\Existing\Task"`. The default is ScheduledDefrag. The test only
inspects it; absence/access denial is a test failure, not permission to create it.
The CLI itself never invokes PowerShell, schtasks, or any external command. Build/
validation scripts are development tooling only.

MIT licensed. No remote machines, task editing/repair/execution, GUI, monitoring,
credential inspection, or network testing.
