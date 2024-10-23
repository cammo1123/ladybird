/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, kleines Filmröllchen <malu.bertsch@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinaryHeap.h>
#include <AK/Singleton.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <AK/WeakPtr.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoopImplementationWindows.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <WinSock2.h>

namespace Core {

struct ThreadData;
class TimeoutSet;

namespace {
thread_local ThreadData* s_thread_data;
}

class EventLoopTimeout {
public:
    static constexpr ssize_t INVALID_INDEX = NumericLimits<ssize_t>::max();

    EventLoopTimeout() { }
    virtual ~EventLoopTimeout() = default;

    virtual void fire(TimeoutSet& timeout_set, MonotonicTime time) = 0;

    MonotonicTime fire_time() const { return m_fire_time; }

    void absolutize(Badge<TimeoutSet>, MonotonicTime current_time)
    {
        m_fire_time = current_time + m_duration;
    }

    ssize_t& index(Badge<TimeoutSet>) { return m_index; }
    void set_index(Badge<TimeoutSet>, ssize_t index) { m_index = index; }

    bool is_scheduled() const { return m_index != INVALID_INDEX; }

protected:
    union {
        AK::Duration m_duration;
        MonotonicTime m_fire_time;
    };

private:
    ssize_t m_index = INVALID_INDEX;
};

class TimeoutSet {
public:
    TimeoutSet() = default;

    Optional<MonotonicTime> next_timer_expiration()
    {
        if (!m_heap.is_empty()) {
            return m_heap.peek_min()->fire_time();
        } else {
            return {};
        }
    }

    void absolutize_relative_timeouts(MonotonicTime current_time)
    {
        for (auto timeout : m_scheduled_timeouts) {
            timeout->absolutize({}, current_time);
            m_heap.insert(timeout);
        }
        m_scheduled_timeouts.clear();
    }

    size_t fire_expired(MonotonicTime current_time)
    {
        size_t fired_count = 0;
        while (!m_heap.is_empty()) {
            auto& timeout = *m_heap.peek_min();

            if (timeout.fire_time() <= current_time) {
                ++fired_count;
                m_heap.pop_min();
                timeout.set_index({}, EventLoopTimeout::INVALID_INDEX);
                timeout.fire(*this, current_time);
            } else {
                break;
            }
        }
        return fired_count;
    }

    void schedule_relative(EventLoopTimeout* timeout)
    {
        timeout->set_index({}, -1 - static_cast<ssize_t>(m_scheduled_timeouts.size()));
        m_scheduled_timeouts.append(timeout);
    }

    void schedule_absolute(EventLoopTimeout* timeout)
    {
        m_heap.insert(timeout);
    }

    void unschedule(EventLoopTimeout* timeout)
    {
        if (timeout->index({}) < 0) {
            size_t i = -1 - timeout->index({});
            size_t j = m_scheduled_timeouts.size() - 1;
            VERIFY(m_scheduled_timeouts[i] == timeout);
            swap(m_scheduled_timeouts[i], m_scheduled_timeouts[j]);
            swap(m_scheduled_timeouts[i]->index({}), m_scheduled_timeouts[j]->index({}));
            (void)m_scheduled_timeouts.take_last();
        } else {
            m_heap.pop(timeout->index({}));
        }
        timeout->set_index({}, EventLoopTimeout::INVALID_INDEX);
    }

    void clear()
    {
        for (auto* timeout : m_heap.nodes_in_arbitrary_order())
            timeout->set_index({}, EventLoopTimeout::INVALID_INDEX);
        m_heap.clear();
        for (auto* timeout : m_scheduled_timeouts)
            timeout->set_index({}, EventLoopTimeout::INVALID_INDEX);
        m_scheduled_timeouts.clear();
    }

private:
    IntrusiveBinaryHeap<
        EventLoopTimeout*,
        decltype([](EventLoopTimeout* a, EventLoopTimeout* b) {
            return a->fire_time() < b->fire_time();
        }),
        decltype([](EventLoopTimeout* timeout, size_t index) {
            timeout->set_index({}, static_cast<ssize_t>(index));
        }),
        8>
        m_heap;
    Vector<EventLoopTimeout*, 8> m_scheduled_timeouts;
};

class EventLoopTimer final : public EventLoopTimeout {
public:
    EventLoopTimer() = default;

    void reload(MonotonicTime const& now) { m_fire_time = now + interval; }

    virtual void fire(TimeoutSet& timeout_set, MonotonicTime current_time) override
    {
        auto strong_owner = owner.strong_ref();

        if (!strong_owner)
            return;

        if (should_reload) {
            MonotonicTime next_fire_time = m_fire_time + interval;
            if (next_fire_time <= current_time) {
                next_fire_time = current_time + interval;
            }
            m_fire_time = next_fire_time;
            if (next_fire_time != current_time) {
                timeout_set.schedule_absolute(this);
            } else {
                // NOTE: Unfortunately we need to treat timeouts with the zero interval in a
                //       special way. TimeoutSet::schedule_absolute for them will result in an
                //       infinite loop. TimeoutSet::schedule_relative, on the other hand, will do a
                //       correct thing of scheduling them for the next iteration of the loop.
                m_duration = {};
                timeout_set.schedule_relative(this);
            }
        }

        // FIXME: While TimerShouldFireWhenNotVisible::Yes prevents the timer callback from being
        //        called, it doesn't allow event loop to sleep since it needs to constantly check if
        //        is_visible_for_timer_purposes changed. A better solution will be to unregister a
        //        timer and register it back again when needed. This also has an added benefit of
        //        making fire_when_not_visible and is_visible_for_timer_purposes obsolete.
        if (fire_when_not_visible == TimerShouldFireWhenNotVisible::Yes || strong_owner->is_visible_for_timer_purposes())
            ThreadEventQueue::current().post_event(*strong_owner, make<TimerEvent>());
    }

    AK::Duration interval;
    bool should_reload { false };
    TimerShouldFireWhenNotVisible fire_when_not_visible { TimerShouldFireWhenNotVisible::No };
    WeakPtr<EventReceiver> owner;
    pthread_t owner_thread { 0 };
    Atomic<bool> is_being_deleted { false };
};

struct ThreadData {
    static ThreadData& the()
    {
        if (!s_thread_data) {
            // FIXME: Don't leak this.
            s_thread_data = new ThreadData;
        }
        return *s_thread_data;
    }

    static ThreadData* for_thread(pthread_t thread_id)
    {
        // auto result = s_thread_data.get(thread_id).value_or(nullptr);
		(void) thread_id;
		TODO();
        return {}; 
    }

    ThreadData()
    {
        pid = GetCurrentProcessId();
        initialize_wake_pipe();
    }

    void initialize_wake_pipe()
    {
        SECURITY_ATTRIBUTES saAttr;
        saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
        saAttr.bInheritHandle = FALSE;
        saAttr.lpSecurityDescriptor = NULL;

        if (!CreatePipe(&wake_pipe_read, &wake_pipe_write, &saAttr, 0))
            VERIFY_NOT_REACHED();

        // Make the write end of the pipe non-inheritable
        if (!SetHandleInformation(wake_pipe_write, HANDLE_FLAG_INHERIT, 0))
            VERIFY_NOT_REACHED();
    }

    // Each thread has its own timers, notifiers and a wake pipe.
    TimeoutSet timeouts;

    // Each thread has its own timers, notifiers and a wake pipe.
    HashMap<int, NonnullOwnPtr<EventLoopTimer>> timers;
    HashTable<Notifier*> notifiers;

    // The wake pipe is used to notify another event loop that someone has called wake(), or a signal has been received.
    // wake() writes 0i32 into the pipe, signals write the signal number (guaranteed non-zero).
    HANDLE wake_pipe_read { NULL };
    HANDLE wake_pipe_write { NULL };

    DWORD pid { 0 };
};

EventLoopImplementationWindows::EventLoopImplementationWindows()
    : m_wake_pipe_read_handle(ThreadData::the().wake_pipe_read)
    , m_wake_pipe_write_handle(ThreadData::the().wake_pipe_write)
{
}

EventLoopImplementationWindows::~EventLoopImplementationWindows() = default;

int EventLoopImplementationWindows::exec()
{
    dbgln("EventLoop: Execing");
    for (;;) {
        dbgln("EventLoop: Execing, waiting for events");
        if (m_exit_requested)
            return m_exit_code;
        pump(PumpMode::WaitForEvents);
    }
    VERIFY_NOT_REACHED();
}

size_t EventLoopImplementationWindows::pump(PumpMode mode)
{
    dbgln("EventLoop: pumppre");
    static_cast<EventLoopManagerWindows&>(EventLoopManager::the()).wait_for_events(mode);
    dbgln("EventLoop: pumppst");
    return ThreadEventQueue::current().process();
}

void EventLoopImplementationWindows::quit(int code)
{
    dbgln("EventLoop: Quitting with code {}", code);
    m_exit_requested = true;
    m_exit_code = code;
}

void EventLoopImplementationWindows::unquit()
{
    dbgln("EventLoop: Unquitting");
    m_exit_requested = false;
    m_exit_code = 0;
}

bool EventLoopImplementationWindows::was_exit_requested() const
{
    dbgln("EventLoop: was_exit_requested");
    return m_exit_requested;
}

void EventLoopImplementationWindows::post_event(EventReceiver& receiver, NonnullOwnPtr<Event>&& event)
{
    dbgln("EventLoop: Post event of");
    m_thread_event_queue.post_event(receiver, move(event));
    if (&m_thread_event_queue != &ThreadEventQueue::current())
        wake();
}

void EventLoopImplementationWindows::wake()
{
    dbgln("EventLoop: wake");
    int wake_event = 0;
    if (!WriteFile(m_wake_pipe_write_handle, &wake_event, sizeof(wake_event), nullptr, nullptr))
        VERIFY_NOT_REACHED();
}

void EventLoopManagerWindows::wait_for_events(EventLoopImplementation::PumpMode mode)
{
    dbgln("EventLoop: wait_for_events");
    auto& thread_data = ThreadData::the();

    WSAEVENT events[thread_data.notifiers.size()];

    SOCKET max_socket = -1;

    for (auto& notifier : thread_data.notifiers) {
        events[notifier->fd()] = WSACreateEvent();

        if (notifier->type() == Notifier::Type::Read)
            WSAEventSelect(notifier->fd(), events[notifier->fd()], FD_READ);
        else if (notifier->type() == Notifier::Type::Write)
            WSAEventSelect(notifier->fd(), events[notifier->fd()], FD_WRITE);
        else
            TODO();
    }

    bool has_pending_events = ThreadEventQueue::current().has_pending_events();

    auto time_at_iteration_start = MonotonicTime::now_coarse();
    thread_data.timeouts.absolutize_relative_timeouts(time_at_iteration_start);

    // Figure out how long to wait at maximum.
    // This mainly depends on the PumpMode and whether we have pending events, but also the next expiring timer.
    int timeout = 0;
    bool should_wait_forever = false;
    if (mode == EventLoopImplementation::PumpMode::WaitForEvents && !has_pending_events) {
        auto next_timer_expiration = thread_data.timeouts.next_timer_expiration();
        if (next_timer_expiration.has_value()) {
            auto computed_timeout = next_timer_expiration.value() - time_at_iteration_start;
            if (computed_timeout.is_negative())
                computed_timeout = AK::Duration::zero();
            i64 true_timeout = computed_timeout.to_milliseconds();
            timeout = static_cast<i32>(min<i64>(AK::NumericLimits<i32>::max(), true_timeout));
        } else {
            should_wait_forever = true;
        }
    }

    if (should_wait_forever) {
        dbgln("EventLoopManagerWindows::wait_for_events: select (max_fd={}, timeout=FOREVER)", max_socket);
    } else {
        dbgln("EventLoopManagerWindows::wait_for_events: select (max_fd={}, timeout={})", max_socket, timeout);
    }

    // select() and wait for file system events, calls to wake(), POSIX signals, or timer expirations.
    DWORD rc = WSAWaitForMultipleEvents(thread_data.notifiers.size() + 1, events, FALSE, should_wait_forever ? WSA_INFINITE : timeout, FALSE);
    auto time_after_poll = MonotonicTime::now_coarse();

    for (size_t i = 0; i < thread_data.notifiers.size(); ++i) {
        if (rc == WSA_WAIT_EVENT_0 + i) {
            dbgln("WSAWaitForMultipleEvents ({}) failed with error: {}", i, WSAGetLastError());
            VERIFY_NOT_REACHED();
        }
    }

    // Handle expired timers.
    thread_data.timeouts.fire_expired(time_after_poll);
}

class SignalHandlers : public RefCounted<SignalHandlers> {
    AK_MAKE_NONCOPYABLE(SignalHandlers);
    AK_MAKE_NONMOVABLE(SignalHandlers);

public:
    SignalHandlers(int signal_number, void (*handle_signal)(int));
    ~SignalHandlers();

    void dispatch();
    int add(Function<void(int)>&& handler);
    bool remove(int handler_id);

    bool is_empty() const
    {
        if (m_calling_handlers) {
            for (auto& handler : m_handlers_pending) {
                if (handler.value)
                    return false; // an add is pending
            }
        }
        return m_handlers.is_empty();
    }

    bool have(int handler_id) const
    {
        if (m_calling_handlers) {
            auto it = m_handlers_pending.find(handler_id);
            if (it != m_handlers_pending.end()) {
                if (!it->value)
                    return false; // a deletion is pending
            }
        }
        return m_handlers.contains(handler_id);
    }

    int m_signal_number;
    void (*m_original_handler)(int); // TODO: can't use sighandler_t?
    HashMap<int, Function<void(int)>> m_handlers;
    HashMap<int, Function<void(int)>> m_handlers_pending;
    bool m_calling_handlers { false };
};

struct SignalHandlersInfo {
    HashMap<int, NonnullRefPtr<SignalHandlers>> signal_handlers;
    int next_signal_id { 0 };
};

static Singleton<SignalHandlersInfo> s_signals;
template<bool create_if_null = true>
inline SignalHandlersInfo* signals_info()
{
    return s_signals.ptr();
}

void EventLoopManagerWindows::dispatch_signal(int signal_number)
{
    auto& info = *signals_info();
    auto handlers = info.signal_handlers.find(signal_number);
    if (handlers != info.signal_handlers.end()) {
        // Make sure we bump the ref count while dispatching the handlers!
        // This allows a handler to unregister/register while the handlers
        // are being called!
        auto handler = handlers->value;
        handler->dispatch();
    }
}

void EventLoopImplementationWindows::notify_forked_and_in_child()
{
    auto& thread_data = ThreadData::the();
    thread_data.timers.clear();
    thread_data.notifiers.clear();
    thread_data.initialize_wake_pipe();
    if (auto* info = signals_info<false>()) {
        info->signal_handlers.clear();
        info->next_signal_id = 0;
    }
    thread_data.pid = GetCurrentProcessId();
}

SignalHandlers::SignalHandlers(int signal_number, void (*handle_signal)(int))
    : m_signal_number(signal_number)
    , m_original_handler(signal(signal_number, handle_signal))
{
}

SignalHandlers::~SignalHandlers()
{
    signal(m_signal_number, m_original_handler);
}

void SignalHandlers::dispatch()
{
    TemporaryChange change(m_calling_handlers, true);
    for (auto& handler : m_handlers)
        handler.value(m_signal_number);
    if (!m_handlers_pending.is_empty()) {
        // Apply pending adds/removes
        for (auto& handler : m_handlers_pending) {
            if (handler.value) {
                auto result = m_handlers.set(handler.key, move(handler.value));
                VERIFY(result == AK::HashSetResult::InsertedNewEntry);
            } else {
                m_handlers.remove(handler.key);
            }
        }
        m_handlers_pending.clear();
    }
}

int SignalHandlers::add(Function<void(int)>&& handler)
{
    int id = ++signals_info()->next_signal_id; // TODO: worry about wrapping and duplicates?
    if (m_calling_handlers)
        m_handlers_pending.set(id, move(handler));
    else
        m_handlers.set(id, move(handler));
    return id;
}

bool SignalHandlers::remove(int handler_id)
{
    VERIFY(handler_id != 0);
    if (m_calling_handlers) {
        auto it = m_handlers.find(handler_id);
        if (it != m_handlers.end()) {
            // Mark pending remove
            m_handlers_pending.set(handler_id, {});
            return true;
        }
        it = m_handlers_pending.find(handler_id);
        if (it != m_handlers_pending.end()) {
            if (!it->value)
                return false; // already was marked as deleted
            it->value = nullptr;
            return true;
        }
        return false;
    }
    return m_handlers.remove(handler_id);
}

void EventLoopManagerWindows::handle_signal(int signal_number)
{
    VERIFY(signal_number != 0);
    auto& thread_data = ThreadData::the();
    // We MUST check if the current pid still matches, because there
    // is a window between fork() and exec() where a signal delivered
    // to our fork could be inadvertently routed to the parent process!
    if (GetCurrentProcessId() == thread_data.pid) {
        DWORD nwritten;
        WriteFile(thread_data.wake_pipe_read, &signal_number, sizeof(signal_number), &nwritten, nullptr);
        if (nwritten < sizeof(signal_number)) {
            perror("WriteFile");
            VERIFY_NOT_REACHED();
        }
    } else {
        // We're a fork who received a signal, reset thread_data.pid.
        thread_data.pid = GetCurrentProcessId();
    }
}

int EventLoopManagerWindows::register_signal(int signal_number, Function<void(int)> handler)
{
    VERIFY(signal_number != 0);
    auto& info = *signals_info();
    auto handlers = info.signal_handlers.find(signal_number);
    if (handlers == info.signal_handlers.end()) {
        auto signal_handlers = adopt_ref(*new SignalHandlers(signal_number, EventLoopManagerWindows::handle_signal));
        auto handler_id = signal_handlers->add(move(handler));
        info.signal_handlers.set(signal_number, move(signal_handlers));
        return handler_id;
    } else {
        return handlers->value->add(move(handler));
    }
}

void EventLoopManagerWindows::unregister_signal(int handler_id)
{
    VERIFY(handler_id != 0);
    int remove_signal_number = 0;
    auto& info = *signals_info();
    for (auto& h : info.signal_handlers) {
        auto& handlers = *h.value;
        if (handlers.remove(handler_id)) {
            if (handlers.is_empty())
                remove_signal_number = handlers.m_signal_number;
            break;
        }
    }
    if (remove_signal_number != 0)
        info.signal_handlers.remove(remove_signal_number);
}

intptr_t EventLoopManagerWindows::register_timer(EventReceiver& object, int milliseconds, bool should_reload, TimerShouldFireWhenNotVisible fire_when_not_visible)
{
    VERIFY(milliseconds >= 0);
    auto& thread_data = ThreadData::the();
    auto timer = new EventLoopTimer;
    timer->owner = object;
    timer->interval = AK::Duration::from_milliseconds(milliseconds);
    timer->reload(MonotonicTime::now_coarse());
    timer->should_reload = should_reload;
    timer->fire_when_not_visible = fire_when_not_visible;
    thread_data.timeouts.schedule_absolute(timer);
    return bit_cast<intptr_t>(timer);
}

void EventLoopManagerWindows::unregister_timer(intptr_t timer_id)
{
    auto* timer = bit_cast<EventLoopTimer*>(timer_id);
    auto thread_data_ptr = ThreadData::for_thread(timer->owner_thread);
    if (!thread_data_ptr)
        return;
    auto& thread_data = *thread_data_ptr;
    auto expected = false;
    if (timer->is_being_deleted.compare_exchange_strong(expected, true, AK::MemoryOrder::memory_order_acq_rel)) {
        if (timer->is_scheduled())
            thread_data.timeouts.unschedule(timer);
        delete timer;
    }
}

void EventLoopManagerWindows::register_notifier(Notifier& notifier)
{
    ThreadData::the().notifiers.set(&notifier);
}

void EventLoopManagerWindows::unregister_notifier(Notifier& notifier)
{
    ThreadData::the().notifiers.remove(&notifier);
}

void EventLoopManagerWindows::did_post_event()
{
}

EventLoopManagerWindows::~EventLoopManagerWindows() = default;

NonnullOwnPtr<EventLoopImplementation> EventLoopManagerWindows::make_implementation()
{
    return adopt_own(*new EventLoopImplementationWindows);
}
}
