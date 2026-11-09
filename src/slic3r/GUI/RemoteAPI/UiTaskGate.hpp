#pragma once
// The F8 gate between Remote API tasks and OrcaSlicer's exports, as a pure class (no wx) so its ordering can be
// tested on its own. GUI thread only.
//
// The periodic auto-backup and the MCP panel's checkpoints export the project while pumping the event queue, and
// load_project / export_3mf show progress dialogs that pump it too. An API task that ran inside such a pump would
// change the model or presets halfway through that work, so a task runs only while the gate is open: no export
// bracketed (backup) and no other API task on the stack.
//
// Strictly first in, first out: a task that arrives while any earlier task is still parked parks behind it, even if
// the gate has opened meanwhile, and the parked tasks run in arrival order from ONE posted drain (not re-posted one
// by one to the back of the event queue, where tasks posted in between would overtake them). The drain stops as soon
// as the gate closes again and leaves the rest parked for the next one.
#include <cstddef>
#include <deque>
#include <functional>
#include <utility>

namespace Slic3r { namespace GUI { namespace RemoteAPI {

class UiTaskGate
{
public:
    using Task = std::function<void()>;
    // Queues a callback on the GUI event loop (wxGetApp().CallAfter in OrcaSlicer).
    using Post = std::function<void(Task)>;

    explicit UiTaskGate(Post post) : m_post(std::move(post)) {}

    // A task's turn on the GUI thread: it runs now when the gate is open and nothing is parked, else it parks.
    void arrive(Task task)
    {
        if (!open() || !m_parked.empty() || m_draining) {
            m_parked.push_back(std::move(task));
            if (open()) request_drain(); // the gate is open but earlier tasks wait for their drain
            return;
        }
        run(task);
        if (open() && !m_parked.empty()) request_drain(); // parked during its own event pump
    }

    // An export's bracket (nests: a depth, so an inner `false` never clears an outer `true`).
    void backup(bool on)
    {
        if (on)
            ++m_backup_depth;
        else if (m_backup_depth > 0)
            --m_backup_depth;
        if (open() && !m_parked.empty()) request_drain();
    }

    bool   open() const { return m_backup_depth == 0 && m_task_depth == 0; }
    bool   task_active() const { return m_task_depth > 0; }
    bool   backup_active() const { return m_backup_depth > 0; }
    size_t parked() const { return m_parked.size(); }

private:
    struct Depth
    {
        int &d;
        explicit Depth(int &d) : d(d) { ++d; }
        ~Depth() { --d; }
    };

    void run(Task &task)
    {
        Depth depth(m_task_depth);
        task();
    }

    void request_drain()
    {
        if (m_drain_posted) return;
        m_drain_posted = true;
        m_post([this] { drain(); }); // posted, not inline: the caller unwinds first
    }

    void drain()
    {
        m_drain_posted = false;
        if (m_draining) return; // a drain further up the stack (inside a task's event pump) carries on
        m_draining = true;
        while (open() && !m_parked.empty()) {
            Task t = std::move(m_parked.front());
            m_parked.pop_front();
            run(t);
        }
        m_draining = false;
        // Closed again with tasks left: the bracket's end (backup(false)) or the next arrival posts the next drain.
    }

    Post             m_post;
    std::deque<Task> m_parked;
    int              m_task_depth   = 0;
    int              m_backup_depth = 0;
    bool             m_drain_posted = false;
    bool             m_draining     = false;
};

}}} // namespace Slic3r::GUI::RemoteAPI
