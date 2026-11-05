#pragma once

#include "action/input_verification.hpp"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomationClient.h>
#include <UIAutomationCoreApi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hmi::action::detail
{
struct InputObservationEvidence
{
    VerificationOutcome outcome = VerificationOutcome::Unavailable;
    std::string reason = "readback_unavailable";
    double before_ms = -1.0;
    double after_ms = -1.0;
    std::wstring font_name;
};

// One read-only MTA operation per TextInput owner. The delivery thread keeps
// control of input and OLE; this worker can never send keys or change a value.
class InputObservationWorker
{
    using Clock = std::chrono::steady_clock;
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

public:
    struct Job
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::atomic<bool> cancelled{false};
        std::atomic<DWORD> thread_id{0};
        std::promise<void> completed;
        HWND window = nullptr;
        HWND focus = nullptr;
        DWORD pid = 0;
        std::wstring text;
        bool before_ready = false;
        bool input_finished = false;
        bool delivered = false;
        bool done = false;
        InputObservationEvidence evidence;
    };
    using Ticket = std::shared_ptr<Job>;

    InputObservationWorker() = default;
    InputObservationWorker(const InputObservationWorker &) = delete;
    InputObservationWorker &operator=(const InputObservationWorker &) = delete;
    ~InputObservationWorker()
    {
        cancel(job_);
        if (!worker_.joinable()) return;
        if (!busy()) worker_.join();
        else worker_.detach(); // Only the shared job and copied values outlive the owner.
    }

    bool busy() const
    {
        return completed_.valid() && completed_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
    }

    Ticket begin(std::wstring text, HWND window, DWORD pid, HWND focus)
    {
        if (busy()) return {};
        if (worker_.joinable()) worker_.join();
        job_.reset();
        const auto started = Clock::now();
        auto job = std::make_shared<Job>();
        job->text = std::move(text);
        job->window = window;
        job->focus = focus;
        job->pid = pid;
        completed_ = job->completed.get_future();
        job_ = job;
        try { worker_ = std::thread([job] { run(job); }); }
        catch (...)
        {
            job_.reset();
            completed_ = {};
            return {};
        }
        std::unique_lock lock(job->mutex);
        const bool ready = job->changed.wait_for(lock, std::chrono::milliseconds(150), [&] { return job->before_ready; });
        job->evidence.before_ms = milliseconds(started);
        if (!ready)
        {
            job->evidence.reason = "before_read_timeout";
            lock.unlock();
            cancel(job);
        }
        return job;
    }

    InputObservationEvidence finish(const Ticket &job, bool delivered)
    {
        if (!job) return {VerificationOutcome::Unavailable, "readback_busy_or_unavailable"};
        const auto started = Clock::now();
        std::unique_lock lock(job->mutex);
        job->delivered = delivered;
        job->input_finished = true;
        job->changed.notify_all();
        if (!job->cancelled.load() && !job->done)
        {
            if (!job->changed.wait_for(lock, std::chrono::milliseconds(300), [&] { return job->done; }))
            {
                job->evidence.outcome = VerificationOutcome::Unavailable;
                job->evidence.reason = "after_read_timeout";
                job->evidence.after_ms = milliseconds(started);
                const auto evidence = job->evidence;
                lock.unlock();
                cancel(job);
                return evidence;
            }
        }
        return job->evidence;
    }

private:
    static double milliseconds(Clock::time_point started)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    }

    static void cancel(const Ticket &job)
    {
        if (!job) return;
        job->cancelled.store(true);
        job->changed.notify_all();
        const DWORD thread = job->thread_id.load();
        if (thread) CoCancelCall(thread, 0);
    }

    static bool matches(const Job &job)
    {
        if (job.cancelled.load() || GetForegroundWindow() != job.window) return false;
        DWORD pid = 0;
        const DWORD thread = GetWindowThreadProcessId(job.window, &pid);
        if (pid != job.pid) return false;
        if (job.focus)
        {
            GUITHREADINFO info{};
            info.cbSize = sizeof(info);
            if (!GetGUIThreadInfo(thread, &info) || info.hwndFocus != job.focus) return false;
        }
        return true;
    }

    static bool range_text(IUIAutomationTextRange *range, std::wstring &out)
    {
        BSTR value = nullptr;
        const HRESULT status = range->GetText(static_cast<int>(maximum_verification_text_units + 1), &value);
        if (FAILED(status)) { SysFreeString(value); return false; }
        const UINT size = value ? SysStringLen(value) : 0;
        if (size > maximum_verification_text_units) { SysFreeString(value); return false; }
        out.assign(value ? value : L"", size);
        SysFreeString(value);
        return true;
    }

    static InputTextSnapshot snapshot(IUIAutomationTextPattern *pattern, std::wstring *font)
    {
        InputTextSnapshot result;
        ComPtr<IUIAutomationTextRange> document;
        ComPtr<IUIAutomationTextRangeArray> selection;
        if (FAILED(pattern->get_DocumentRange(&document)) || !document ||
            !range_text(document.Get(), result.text)) return result;
        result.available = result.complete = true;
        int count = 0;
        ComPtr<IUIAutomationTextRange> selected;
        if (FAILED(pattern->GetSelection(&selection)) || !selection ||
            FAILED(selection->get_Length(&count)) || count != 1 ||
            FAILED(selection->GetElement(0, &selected)) || !selected) return result;
        ComPtr<IUIAutomationTextRange> prefix;
        std::wstring start_text, end_text, rechecked;
        if (FAILED(document->Clone(&prefix)) ||
            FAILED(prefix->MoveEndpointByRange(TextPatternRangeEndpoint_End, selected.Get(), TextPatternRangeEndpoint_Start)) ||
            !range_text(prefix.Get(), start_text) ||
            FAILED(prefix->MoveEndpointByRange(TextPatternRangeEndpoint_End, selected.Get(), TextPatternRangeEndpoint_End)) ||
            !range_text(prefix.Get(), end_text) || !range_text(document.Get(), rechecked) ||
            rechecked != result.text || start_text.size() > end_text.size() ||
            result.text.compare(0, start_text.size(), start_text) != 0 ||
            result.text.compare(0, end_text.size(), end_text) != 0) return result;
        result.selection_begin = start_text.size();
        result.selection_end = end_text.size();
        result.selection_available = true;
        if (font)
        {
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(selected->GetAttributeValue(UIA_FontNameAttributeId, &value)) &&
                value.vt == VT_BSTR && value.bstrVal && SysStringLen(value.bstrVal) <= 256)
                font->assign(value.bstrVal, SysStringLen(value.bstrVal));
            VariantClear(&value);
        }
        return result;
    }

    static void run(const Ticket &job) noexcept
    {
        job->thread_id.store(GetCurrentThreadId());
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoEnableCallCancellation(nullptr);
        try
        {
            InputTextSnapshot before;
            ComPtr<IUIAutomation> automation;
            ComPtr<IUIAutomationElement> element;
            ComPtr<IUIAutomationTextPattern> pattern;
            std::wstring font;
            bool ready = SUCCEEDED(com) && matches(*job) &&
                SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)));
            if (!ready && SUCCEEDED(com) && matches(*job))
                ready = SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)));
            ComPtr<IUIAutomation2> timed;
            if (automation && SUCCEEDED(automation.As(&timed)))
            {
                timed->put_ConnectionTimeout(100);
                timed->put_TransactionTimeout(100);
            }
            int pid = 0;
            ready = ready && automation && SUCCEEDED(automation->GetFocusedElement(&element)) && element &&
                SUCCEEDED(element->get_CurrentProcessId(&pid)) && static_cast<DWORD>(pid) == job->pid &&
                SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))) && pattern;
            if (ready)
            {
                ComPtr<IUIAutomationTextPattern2> text2;
                if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&text2))) && text2)
                {
                    BOOL active = FALSE;
                    ComPtr<IUIAutomationTextRange> caret;
                    if (FAILED(text2->GetCaretRange(&active, &caret)) || !active) ready = false;
                }
            }
            if (ready && matches(*job)) before = snapshot(pattern.Get(), &font);
            {
                std::lock_guard lock(job->mutex);
                job->before_ready = true;
                job->evidence.font_name = std::move(font);
                if (!job->cancelled.load()) job->evidence.reason = before.selection_available ?
                    "awaiting_delivery" : "text_or_selection_unavailable";
            }
            job->changed.notify_all();
            if (before.selection_available && matches(*job))
            {
                std::unique_lock lock(job->mutex);
                job->changed.wait(lock, [&] { return job->input_finished || job->cancelled.load(); });
                const bool delivered = job->delivered;
                lock.unlock();
                if (delivered && matches(*job))
                {
                    const auto started = Clock::now();
                    InputVerificationResult verification;
                    do
                    {
                        ComPtr<IUIAutomationElement> current;
                        BOOL same = FALSE;
                        const bool same_control = matches(*job) &&
                            SUCCEEDED(automation->GetFocusedElement(&current)) && current &&
                            SUCCEEDED(automation->CompareElements(element.Get(), current.Get(), &same)) && same;
                        if (!same_control) { verification = {VerificationOutcome::Unavailable, "control_changed"}; break; }
                        const auto after = snapshot(pattern.Get(), nullptr);
                        ComPtr<IUIAutomationElement> rechecked;
                        BOOL still_same = FALSE;
                        if (!matches(*job) || FAILED(automation->GetFocusedElement(&rechecked)) || !rechecked ||
                            FAILED(automation->CompareElements(element.Get(), rechecked.Get(), &still_same)) || !still_same)
                        { verification = {VerificationOutcome::Unavailable, "control_changed"}; break; }
                        verification = verify_input_change(before, after, job->text, true);
                        if (verification.outcome == VerificationOutcome::Verified || job->cancelled.load()) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    } while (milliseconds(started) < 200.0);
                    std::lock_guard evidence_lock(job->mutex);
                    if (!job->cancelled.load())
                    {
                        job->evidence.outcome = verification.outcome;
                        job->evidence.reason = verification.reason;
                        job->evidence.after_ms = milliseconds(started);
                    }
                }
            }
        }
        catch (...)
        {
            std::lock_guard lock(job->mutex);
            if (!job->cancelled.load()) job->evidence.reason = "readback_failed";
        }
        CoDisableCallCancellation(nullptr);
        if (SUCCEEDED(com)) CoUninitialize();
        {
            std::lock_guard lock(job->mutex);
            job->before_ready = job->done = true;
        }
        job->changed.notify_all();
        try { job->completed.set_value(); } catch (...) { }
    }

    Ticket job_;
    std::thread worker_;
    std::future<void> completed_;
};
}
