#include "model.hpp"
#include <chrono>
#include <memory>
#include <sstream>
#include <taskschd.h>
#include <winevt.h>
#include <wrl/client.h>

namespace stfe {
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr, const char *operation) {
    if (FAILED(hr))
        throw Failure(static_cast<uint32_t>(hr), operation);
}
class Apartment {
  public:
    Apartment() {
        check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM initialization");
    }
    ~Apartment() {
        CoUninitialize();
    }
    Apartment(const Apartment &) = delete;
    Apartment &operator=(const Apartment &) = delete;
};
class Bstr {
    BSTR value_ = nullptr;

  public:
    Bstr() = default;
    explicit Bstr(const std::wstring &s) : value_(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {
        if (!value_)
            throw std::bad_alloc();
    }
    ~Bstr() {
        SysFreeString(value_);
    }
    Bstr(const Bstr &) = delete;
    Bstr &operator=(const Bstr &) = delete;
    BSTR get() const {
        return value_;
    }
    BSTR *put() {
        return &value_;
    }
    std::string str() const {
        return value_ ? utf8(std::wstring(value_, SysStringLen(value_))) : std::string{};
    }
};
template <class T, class F> static std::string string_property(T *object, F getter, const char *label) {
    if (!object)
        throw Failure(ERROR_INVALID_DATA, label);
    Bstr value;
    check((object->*getter)(value.put()), label);
    return value.str();
}
template <class T, class F> static bool bool_property(T *object, F getter, const char *label) {
    if (!object)
        throw Failure(ERROR_INVALID_DATA, label);
    VARIANT_BOOL value = VARIANT_FALSE;
    check((object->*getter)(&value), label);
    return value != VARIANT_FALSE;
}
static std::string local_date(DATE date) {
    if (date == 0 || date <= 2)
        return {}; // Scheduler's 1899-12-30 sentinel is not an execution.
    SYSTEMTIME time{};
    if (!VariantTimeToSystemTime(date, &time))
        throw Failure(ERROR_INVALID_DATA, "Task Scheduler date");
    char buffer[64]{};
    sprintf_s(buffer, "%04u-%02u-%02uT%02u:%02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour,
              time.wMinute, time.wSecond);
    return buffer;
}
struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit NativeHandle(HANDLE v) : value(v) {}
    ~NativeHandle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    NativeHandle(const NativeHandle &) = delete;
    NativeHandle &operator=(const NativeHandle &) = delete;
};
void probe_action(Action &a) {
    if (a.type != TASK_ACTION_EXEC)
        return;
    const auto input = wide(a.command);
    a.probe = "skipped_ambiguous_path";
    if (input.empty() || input.find(L'\0') != std::wstring::npos)
        return;
    const DWORD n = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (!n || n > 32768) {
        a.probe = "unavailable";
        return;
    }
    std::wstring path(n, L'\0');
    if (ExpandEnvironmentStringsW(input.c_str(), path.data(), n) != n) {
        a.probe = "unavailable";
        return;
    }
    path.resize(n - 1);
    if (path.rfind(L'\\', 0) == 0 || path.find(L"://") != std::wstring::npos) {
        a.probe = "skipped_remote_or_device_path";
        return;
    }
    if (path.size() < 4 || path[1] != L':' || path[2] != L'\\' ||
        path.find_first_of(L"%\"/*?") != std::wstring::npos || path.find(L':', 2) != std::wstring::npos)
        return;
    // Reject every ambiguous component before touching the filesystem.
    for (size_t begin = 3; begin < path.size();) {
        const size_t end = path.find(L'\\', begin);
        const auto part = path.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
        if (part.empty() || part == L"." || part == L"..")
            return;
        if (end == std::wstring::npos)
            break;
        begin = end + 1;
        if (begin == path.size())
            return;
    }
    const wchar_t drive[] = {path[0], L':', L'\0'};
    wchar_t device[4096]{};
    if (!QueryDosDeviceW(drive, device, 4096) ||
        std::wstring(device).rfind(L"\\Device\\HarddiskVolume", 0) != 0) {
        a.probe = "skipped_nonlocal_drive";
        return;
    }
    const std::wstring root = path.substr(0, 3);
    if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
        a.probe = "skipped_nonlocal_drive";
        return;
    }
    // Hold ancestors without write/delete sharing while inspecting descendants. Reparse points
    // are never followed, including OneDrive placeholders, junctions, and symbolic links.
    std::vector<std::unique_ptr<NativeHandle>> ancestors;
    size_t end = 2;
    while (end < path.size()) {
        const size_t next = path.find(L'\\', end + 1);
        const std::wstring part =
            path.substr(end + 1, next == std::wstring::npos ? std::wstring::npos : next - end - 1);
        if (part.empty() || part == L"." || part == L"..")
            return;
        const std::wstring current = next == std::wstring::npos ? path : path.substr(0, next);
        auto handle = std::make_unique<NativeHandle>(
            CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (handle->value == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            a.probe =
                error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? "missing" : "unavailable";
            return;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle->value, &info)) {
            a.probe = "unavailable";
            return;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            a.probe = "skipped_reparse_point";
            return;
        }
        if (next == std::wstring::npos) {
            a.probe = info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? "is_directory" : "exists";
            return;
        }
        ancestors.push_back(std::move(handle));
        end = next;
    }
}
class Evt {
    EVT_HANDLE value_ = nullptr;

  public:
    explicit Evt(EVT_HANDLE value = nullptr) : value_(value) {}
    ~Evt() {
        if (value_)
            EvtClose(value_);
    }
    Evt(const Evt &) = delete;
    Evt &operator=(const Evt &) = delete;
    EVT_HANDLE get() const {
        return value_;
    }
};
static std::string event_string(const EVT_VARIANT &v) {
    return (v.Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeString && v.StringVal ? utf8(v.StringVal)
                                                                               : std::string{};
}
static std::string event_time(const EVT_VARIANT &v) {
    if ((v.Type & EVT_VARIANT_TYPE_MASK) != EvtVarTypeFileTime)
        return {};
    const FILETIME ft{static_cast<DWORD>(v.FileTimeVal), static_cast<DWORD>(v.FileTimeVal >> 32U)};
    SYSTEMTIME t{};
    if (!FileTimeToSystemTime(&ft, &t))
        return {};
    char buffer[64]{};
    sprintf_s(buffer, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
              t.wSecond, t.wMilliseconds);
    return buffer;
}
static std::string variant_text(const EVT_VARIANT &v) {
    switch (v.Type & EVT_VARIANT_TYPE_MASK) {
    case EvtVarTypeString:
        return event_string(v);
    case EvtVarTypeUInt32:
    case EvtVarTypeHexInt32:
        return std::to_string(v.UInt32Val);
    case EvtVarTypeUInt64:
    case EvtVarTypeHexInt64:
        return std::to_string(v.UInt64Val);
    case EvtVarTypeInt32:
        return std::to_string(v.Int32Val);
    default:
        return {};
    }
}
static void history(Snapshot &s) {
    constexpr const wchar_t *channel = L"Microsoft-Windows-TaskScheduler/Operational";
    Evt config(EvtOpenChannelConfig(nullptr, channel, 0));
    if (!config.get())
        throw Failure(GetLastError(), "Open Operational channel configuration");
    EVT_VARIANT enabled{};
    DWORD used = 0;
    if (!EvtGetChannelConfigProperty(config.get(), EvtChannelConfigEnabled, 0, sizeof(enabled), &enabled,
                                     &used))
        throw Failure(GetLastError(), "Read Operational channel enabled state");
    if (enabled.Type != EvtVarTypeBoolean)
        throw Failure(ERROR_INVALID_DATA, "Operational channel enabled state");
    if (!enabled.BooleanVal)
        s.warnings.emplace_back(
            "Operational channel is disabled; querying retained events only, without enabling the channel.");
    const auto path = wide(s.path);
    const auto query = history_query(path);
    if (path.find(L'\'') != std::wstring::npos && path.find(L'"') != std::wstring::npos)
        s.warnings.emplace_back("Task path contains both quote styles; history uses bounded time filtering "
                                "and exact client-side task matching.");
    Evt events(
        EvtQuery(nullptr, channel, query.c_str(),
                 static_cast<DWORD>(EvtQueryChannelPath) | static_cast<DWORD>(EvtQueryReverseDirection)));
    if (!events.get())
        throw Failure(GetLastError(), "Query Operational history");
    const wchar_t *paths[] = {L"Event/System/EventID",
                              L"Event/System/EventRecordID",
                              L"Event/System/TimeCreated/@SystemTime",
                              L"Event/EventData/Data[@Name='TaskName']",
                              L"Event/EventData/Data[@Name='TaskInstanceId']",
                              L"Event/EventData/Data[@Name='InstanceId']",
                              L"Event/EventData/Data[@Name='ResultCode']",
                              L"Event/EventData/Data[@Name='ErrorCode']",
                              L"Event/EventData/Data[@Name='ActionName']",
                              L"Event/EventData/Data[@Name='UserContext']"};
    Evt context(EvtCreateRenderContext(10, paths, EvtRenderContextValues));
    if (!context.get())
        throw Failure(GetLastError(), "Create event render context");
    Evt publisher(EvtOpenPublisherMetadata(nullptr, L"Microsoft-Windows-TaskScheduler", nullptr, 0, 0));
    if (!publisher.get())
        s.warnings.emplace_back("Local Task Scheduler event message resources are unavailable; structured "
                                "event fields remain usable.");
    std::vector<Event> candidates;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    size_t examined = 0;
    for (; examined < 512 && candidates.size() < 100; ++examined) {
        if (std::chrono::steady_clock::now() >= deadline) {
            s.warnings.emplace_back(
                "Event history query reached its three-second deadline; results may be incomplete.");
            break;
        }
        EVT_HANDLE raw = nullptr;
        DWORD count = 0;
        if (!EvtNext(events.get(), 1, &raw, 100, 0, &count)) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_ITEMS)
                break;
            if (error == ERROR_TIMEOUT) {
                s.warnings.emplace_back("Event query timed out; results may be incomplete.");
                break;
            }
            throw Failure(error, "Read Operational history");
        }
        Evt event(raw);
        DWORD bytes = 0, properties = 0;
        if (EvtRender(context.get(), event.get(), EvtRenderEventValues, 0, nullptr, &bytes, &properties) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            throw Failure(ERROR_INVALID_DATA, "Size event properties");
        if (bytes > 1024 * 1024) {
            s.warnings.emplace_back("Oversized event skipped.");
            continue;
        }
        // Use aligned storage for EVT_VARIANT; the API's required byte count includes strings.
        std::vector<uint64_t> buffer((static_cast<size_t>(bytes) + 7) / 8);
        if (!EvtRender(context.get(), event.get(), EvtRenderEventValues, bytes, buffer.data(), &bytes,
                       &properties))
            throw Failure(GetLastError(), "Render event properties");
        if (properties != 10) {
            s.warnings.emplace_back("Unexpected event property count; event skipped.");
            continue;
        }
        const auto *v = reinterpret_cast<const EVT_VARIANT *>(buffer.data());
        Event e;
        if ((v[0].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeUInt16)
            e.id = v[0].UInt16Val;
        else if ((v[0].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeUInt32)
            e.id = v[0].UInt32Val;
        if ((v[1].Type & EVT_VARIANT_TYPE_MASK) == EvtVarTypeUInt64)
            e.record = static_cast<int64_t>(v[1].UInt64Val);
        e.time = event_time(v[2]);
        e.task = event_string(v[3]);
        e.instance = event_string(v[4]);
        if (e.instance.empty())
            e.instance = event_string(v[5]);
        const char *names[] = {"result_code", "error_code", "action_name", "user_context"};
        for (size_t i = 6; i < 10; ++i) {
            const auto text = variant_text(v[i]);
            if (!text.empty())
                e.data.emplace(names[i - 6], text);
        }
        if (publisher.get()) {
            DWORD length = 0;
            if (!EvtFormatMessage(publisher.get(), event.get(), 0, 0, nullptr, EvtFormatMessageEvent, 0,
                                  nullptr, &length) &&
                GetLastError() == ERROR_INSUFFICIENT_BUFFER && length <= 65536) {
                std::vector<wchar_t> message(length);
                if (EvtFormatMessage(publisher.get(), event.get(), 0, 0, nullptr, EvtFormatMessageEvent,
                                     length, message.data(), &length))
                    e.message = utf8(message.data());
            }
        }
        // Defense in depth: do not trust the query to establish task identity.
        Snapshot matched;
        matched.path = s.path;
        correlate(matched, {e});
        if (!matched.events.empty())
            candidates.push_back(std::move(e));
    }
    if (examined == 512 || candidates.size() == 100)
        s.warnings.emplace_back("Event count limit reached; history may be incomplete.");
    correlate(s, candidates);
    s.history_status = enabled.BooleanVal ? "available" : "disabled";
}
static Trigger read_trigger(ITrigger *t) {
    if (!t)
        throw Failure(ERROR_INVALID_DATA, "Null trigger interface");
    Trigger out;
    TASK_TRIGGER_TYPE2 type{};
    check(t->get_Type(&type), "Trigger type");
    out.type = type;
    out.enabled = bool_property(t, &ITrigger::get_Enabled, "Trigger enabled");
    out.id = string_property(t, &ITrigger::get_Id, "Trigger ID");
    out.start = string_property(t, &ITrigger::get_StartBoundary, "Trigger start boundary");
    out.end = string_property(t, &ITrigger::get_EndBoundary, "Trigger end boundary");
    out.execution_limit = string_property(t, &ITrigger::get_ExecutionTimeLimit, "Trigger execution limit");
    ComPtr<IRepetitionPattern> repetition;
    check(t->get_Repetition(&repetition), "Trigger repetition");
    if (repetition) {
        out.repetition_interval =
            string_property(repetition.Get(), &IRepetitionPattern::get_Interval, "Repetition interval");
        out.repetition_duration =
            string_property(repetition.Get(), &IRepetitionPattern::get_Duration, "Repetition duration");
        out.details["stop_at_duration_end"] =
            bool_property(repetition.Get(), &IRepetitionPattern::get_StopAtDurationEnd, "Repetition stop");
    }
    switch (type) {
    case TASK_TRIGGER_EVENT: {
        ComPtr<IEventTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Event trigger interface");
        out.details["subscription"] =
            string_property(p.Get(), &IEventTrigger::get_Subscription, "Event subscription");
        out.details["delay"] = string_property(p.Get(), &IEventTrigger::get_Delay, "Event delay");
        break;
    }
    case TASK_TRIGGER_TIME: {
        ComPtr<ITimeTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Time trigger interface");
        out.details["random_delay"] =
            string_property(p.Get(), &ITimeTrigger::get_RandomDelay, "Time random delay");
        break;
    }
    case TASK_TRIGGER_DAILY: {
        ComPtr<IDailyTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Daily trigger interface");
        short n = 0;
        check(p->get_DaysInterval(&n), "Days interval");
        out.details["days_interval"] = n;
        out.details["random_delay"] =
            string_property(p.Get(), &IDailyTrigger::get_RandomDelay, "Daily random delay");
        break;
    }
    case TASK_TRIGGER_WEEKLY: {
        ComPtr<IWeeklyTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Weekly trigger interface");
        short days = 0, weeks = 0;
        check(p->get_DaysOfWeek(&days), "Weekly days");
        check(p->get_WeeksInterval(&weeks), "Weekly interval");
        out.details["days_of_week_mask"] = days;
        out.details["weeks_interval"] = weeks;
        out.details["random_delay"] =
            string_property(p.Get(), &IWeeklyTrigger::get_RandomDelay, "Weekly random delay");
        break;
    }
    case TASK_TRIGGER_MONTHLY: {
        ComPtr<IMonthlyTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Monthly trigger interface");
        long days = 0;
        short months = 0;
        check(p->get_DaysOfMonth(&days), "Monthly days");
        check(p->get_MonthsOfYear(&months), "Monthly months");
        out.details["days_of_month_mask"] = static_cast<int64_t>(days);
        out.details["months_of_year_mask"] = months;
        out.details["last_day_of_month"] =
            bool_property(p.Get(), &IMonthlyTrigger::get_RunOnLastDayOfMonth, "Monthly last day");
        out.details["random_delay"] =
            string_property(p.Get(), &IMonthlyTrigger::get_RandomDelay, "Monthly random delay");
        break;
    }
    case TASK_TRIGGER_MONTHLYDOW: {
        ComPtr<IMonthlyDOWTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Monthly weekday trigger interface");
        short days = 0, months = 0, weeks = 0;
        check(p->get_DaysOfWeek(&days), "Monthly weekdays");
        check(p->get_MonthsOfYear(&months), "Monthly months");
        check(p->get_WeeksOfMonth(&weeks), "Monthly weeks");
        out.details["days_of_week_mask"] = days;
        out.details["months_of_year_mask"] = months;
        out.details["weeks_of_month_mask"] = weeks;
        out.details["last_week"] =
            bool_property(p.Get(), &IMonthlyDOWTrigger::get_RunOnLastWeekOfMonth, "Monthly last week");
        break;
    }
    case TASK_TRIGGER_LOGON: {
        ComPtr<ILogonTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Logon trigger interface");
        out.details["user_id"] = string_property(p.Get(), &ILogonTrigger::get_UserId, "Logon user");
        out.details["delay"] = string_property(p.Get(), &ILogonTrigger::get_Delay, "Logon delay");
        break;
    }
    case TASK_TRIGGER_BOOT: {
        ComPtr<IBootTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Boot trigger interface");
        out.details["delay"] = string_property(p.Get(), &IBootTrigger::get_Delay, "Boot delay");
        break;
    }
    case TASK_TRIGGER_REGISTRATION: {
        ComPtr<IRegistrationTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Registration trigger interface");
        out.details["delay"] =
            string_property(p.Get(), &IRegistrationTrigger::get_Delay, "Registration delay");
        break;
    }
    case TASK_TRIGGER_SESSION_STATE_CHANGE: {
        ComPtr<ISessionStateChangeTrigger> p;
        check(t->QueryInterface(IID_PPV_ARGS(&p)), "Session trigger interface");
        TASK_SESSION_STATE_CHANGE_TYPE state = TASK_CONSOLE_CONNECT;
        check(p->get_StateChange(&state), "Session state change");
        out.details["state_change_value"] = static_cast<int>(state);
        out.details["user_id"] =
            string_property(p.Get(), &ISessionStateChangeTrigger::get_UserId, "Session user");
        out.details["delay"] =
            string_property(p.Get(), &ISessionStateChangeTrigger::get_Delay, "Session delay");
        break;
    }
    default:
        break;
    }
    return out;
}
Snapshot acquire(const std::wstring &path) {
    Apartment apartment;
    ComPtr<ITaskService> service;
    check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)),
          "Create Task Scheduler service");
    if (!service)
        throw Failure(ERROR_INVALID_DATA, "Null Task Scheduler service");
    VARIANT empty{};
    VariantInit(&empty);
    check(service->Connect(empty, empty, empty, empty), "Connect to local Task Scheduler");
    const size_t split = path.rfind(L'\\');
    if (split == std::wstring::npos || split + 1 == path.size())
        throw Failure(ERROR_INVALID_NAME, "Task path");
    Bstr folder_path(split == 0 ? L"\\" : path.substr(0, split));
    Bstr task_name(path.substr(split + 1));
    ComPtr<ITaskFolder> folder;
    check(service->GetFolder(folder_path.get(), &folder),
          "Open task folder (missing or inaccessible folder)");
    if (!folder)
        throw Failure(ERROR_INVALID_DATA, "Null task folder");
    ComPtr<IRegisteredTask> task;
    check(folder->GetTask(task_name.get(), &task), "Open task (missing or inaccessible task)");
    Snapshot s;
    s.path = string_property(task.Get(), &IRegisteredTask::get_Path, "Task path");
    s.name = string_property(task.Get(), &IRegisteredTask::get_Name, "Task name");
    s.enabled = bool_property(task.Get(), &IRegisteredTask::get_Enabled, "Task enabled");
    TASK_STATE state{};
    check(task->get_State(&state), "Task state");
    s.state = state;
    LONG result = 0;
    check(task->get_LastTaskResult(&result), "Last task result");
    s.result = static_cast<uint32_t>(result);
    DATE last = 0, next = 0;
    check(task->get_LastRunTime(&last), "Last run time");
    check(task->get_NextRunTime(&next), "Next run time");
    s.last_run = local_date(last);
    s.next_run = local_date(next);
    LONG missed = 0;
    const HRESULT missed_hr = task->get_NumberOfMissedRuns(&missed);
    if (SUCCEEDED(missed_hr))
        s.missed = missed;
    else {
        s.missed = -1;
        s.warnings.emplace_back("Missed-run count unavailable: " + hex(static_cast<uint32_t>(missed_hr)));
    }
    ComPtr<ITaskDefinition> def;
    check(task->get_Definition(&def), "Read task definition (task may have disappeared)");
    if (!def)
        throw Failure(ERROR_INVALID_DATA, "Null task definition");
    ComPtr<IPrincipal> principal;
    check(def->get_Principal(&principal), "Task principal");
    s.user = string_property(principal.Get(), &IPrincipal::get_UserId, "Principal user");
    s.group = string_property(principal.Get(), &IPrincipal::get_GroupId, "Principal group");
    TASK_LOGON_TYPE logon{};
    TASK_RUNLEVEL_TYPE level{};
    check(principal->get_LogonType(&logon), "Principal logon type");
    check(principal->get_RunLevel(&level), "Principal run level");
    s.logon = logon;
    s.run_level = level;
    ComPtr<ITaskSettings> settings;
    check(def->get_Settings(&settings), "Task settings");
    s.battery_start = bool_property(settings.Get(), &ITaskSettings::get_DisallowStartIfOnBatteries,
                                    "Battery start condition");
    s.battery_stop =
        bool_property(settings.Get(), &ITaskSettings::get_StopIfGoingOnBatteries, "Battery stop condition");
    s.idle = bool_property(settings.Get(), &ITaskSettings::get_RunOnlyIfIdle, "Idle condition");
    s.network =
        bool_property(settings.Get(), &ITaskSettings::get_RunOnlyIfNetworkAvailable, "Network condition");
    s.start_available =
        bool_property(settings.Get(), &ITaskSettings::get_StartWhenAvailable, "Start when available");
    s.wake = bool_property(settings.Get(), &ITaskSettings::get_WakeToRun, "Wake to run");
    s.demand = bool_property(settings.Get(), &ITaskSettings::get_AllowDemandStart, "Demand start");
    s.hard_terminate =
        bool_property(settings.Get(), &ITaskSettings::get_AllowHardTerminate, "Hard terminate setting");
    s.limit = string_property(settings.Get(), &ITaskSettings::get_ExecutionTimeLimit, "Execution limit");
    s.additional_settings["restart_interval"] =
        string_property(settings.Get(), &ITaskSettings::get_RestartInterval, "Restart interval");
    int restart_count = 0;
    check(settings->get_RestartCount(&restart_count), "Restart count");
    s.additional_settings["restart_count"] = restart_count;
    ComPtr<ITaskSettings2> settings2;
    if (SUCCEEDED(settings.As(&settings2)) && settings2) {
        s.additional_settings["use_unified_scheduling_engine"] = bool_property(
            settings2.Get(), &ITaskSettings2::get_UseUnifiedSchedulingEngine, "Unified scheduling engine");
        s.additional_settings["disallow_start_on_remote_app_session"] =
            bool_property(settings2.Get(), &ITaskSettings2::get_DisallowStartOnRemoteAppSession,
                          "RemoteApp session condition");
    } else
        s.warnings.emplace_back(
            "ITaskSettings2 unavailable; extended scheduling conditions were not inspected.");
    ComPtr<ITaskSettings3> settings3;
    s.additional_settings["maintenance"] = Json();
    if (SUCCEEDED(settings.As(&settings3)) && settings3) {
        ComPtr<IMaintenanceSettings> maintenance;
        const HRESULT hr = settings3->get_MaintenanceSettings(&maintenance);
        if (SUCCEEDED(hr) && maintenance) {
            s.additional_settings["maintenance"] = Json::Object{
                {"period",
                 string_property(maintenance.Get(), &IMaintenanceSettings::get_Period, "Maintenance period")},
                {"deadline", string_property(maintenance.Get(), &IMaintenanceSettings::get_Deadline,
                                             "Maintenance deadline")},
                {"exclusive", bool_property(maintenance.Get(), &IMaintenanceSettings::get_Exclusive,
                                            "Maintenance exclusive")}};
        } else if (FAILED(hr))
            s.warnings.emplace_back("Maintenance settings unavailable: " + hex(static_cast<uint32_t>(hr)));
    } else
        s.warnings.emplace_back("ITaskSettings3 unavailable; maintenance scheduling was not inspected.");
    TASK_INSTANCES_POLICY policy{};
    check(settings->get_MultipleInstances(&policy), "Multiple-instance policy");
    s.instances = policy;
    int priority = 0;
    check(settings->get_Priority(&priority), "Priority");
    s.priority = priority;
    ComPtr<IIdleSettings> idle;
    check(settings->get_IdleSettings(&idle), "Idle settings");
    s.idle_duration = string_property(idle.Get(), &IIdleSettings::get_IdleDuration, "Idle duration");
    s.idle_wait = string_property(idle.Get(), &IIdleSettings::get_WaitTimeout, "Idle wait timeout");
    s.idle_stop = bool_property(idle.Get(), &IIdleSettings::get_StopOnIdleEnd, "Stop on idle end");
    s.idle_restart = bool_property(idle.Get(), &IIdleSettings::get_RestartOnIdle, "Restart on idle");
    ComPtr<INetworkSettings> network;
    check(settings->get_NetworkSettings(&network), "Network settings");
    if (network) {
        s.network_name = string_property(network.Get(), &INetworkSettings::get_Name, "Network profile name");
        s.network_id = string_property(network.Get(), &INetworkSettings::get_Id, "Network profile ID");
    }
    ComPtr<IActionCollection> actions;
    check(def->get_Actions(&actions), "Actions");
    if (!actions)
        throw Failure(ERROR_INVALID_DATA, "Null action collection");
    LONG count = 0;
    check(actions->get_Count(&count), "Action count");
    if (count < 0 || count > 1024)
        throw Failure(ERROR_INVALID_DATA, "Action count bounds");
    for (LONG i = 1; i <= count; ++i) {
        ComPtr<IAction> action;
        check(actions->get_Item(i, &action), "Action item");
        if (!action)
            throw Failure(ERROR_INVALID_DATA, "Null action interface");
        Action a;
        TASK_ACTION_TYPE type{};
        check(action->get_Type(&type), "Action type");
        a.type = type;
        a.id = string_property(action.Get(), &IAction::get_Id, "Action ID");
        if (type == TASK_ACTION_EXEC) {
            ComPtr<IExecAction> p;
            check(action.As(&p), "Executable action interface");
            a.command = string_property(p.Get(), &IExecAction::get_Path, "Executable path");
            a.arguments = string_property(p.Get(), &IExecAction::get_Arguments, "Arguments");
            a.directory = string_property(p.Get(), &IExecAction::get_WorkingDirectory, "Working directory");
            probe_action(a);
        } else if (type == TASK_ACTION_COM_HANDLER) {
            ComPtr<IComHandlerAction> p;
            check(action.As(&p), "COM handler interface");
            a.class_id = string_property(p.Get(), &IComHandlerAction::get_ClassId, "COM handler class ID");
            a.data = string_property(p.Get(), &IComHandlerAction::get_Data, "COM handler data");
        } else if (type == TASK_ACTION_SHOW_MESSAGE) {
            ComPtr<IShowMessageAction> p;
            check(action.As(&p), "Legacy message interface");
            a.command = string_property(p.Get(), &IShowMessageAction::get_Title, "Message title");
            a.data = string_property(p.Get(), &IShowMessageAction::get_MessageBody, "Message body");
        } else if (type == TASK_ACTION_SEND_EMAIL) {
            ComPtr<IEmailAction> p;
            check(action.As(&p), "Legacy email interface");
            a.command =
                string_property(p.Get(), &IEmailAction::get_Server, "Email server (configuration only)");
            a.data = string_property(p.Get(), &IEmailAction::get_Subject, "Email subject");
            s.warnings.emplace_back(
                "Legacy email action is deprecated; body, recipients, and attachments are not inspected.");
        } else
            s.warnings.emplace_back("Unknown action type: " + std::to_string(a.type));
        s.actions.push_back(std::move(a));
    }
    ComPtr<ITriggerCollection> triggers;
    check(def->get_Triggers(&triggers), "Triggers");
    if (!triggers)
        throw Failure(ERROR_INVALID_DATA, "Null trigger collection");
    check(triggers->get_Count(&count), "Trigger count");
    if (count < 0 || count > 1024)
        throw Failure(ERROR_INVALID_DATA, "Trigger count bounds");
    for (LONG i = 1; i <= count; ++i) {
        ComPtr<ITrigger> t;
        check(triggers->get_Item(i, &t), "Trigger item");
        s.triggers.push_back(read_trigger(t.Get()));
    }
    acquire_history(s, history);
    return s;
}
} // namespace stfe
