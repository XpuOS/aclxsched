#include "xsched/utils/log.h"
#include "xsched/utils/xassert.h"
#include "xsched/sched/policy/spup.h"

using namespace std::chrono;
using namespace xsched::sched;

StrictProcessUtilizationPartitionPolicy::StrictProcessUtilizationPartitionPolicy()
    : Policy(kPolicyStrictProcessUtilizationPartition)
{
    timeslice_end_ = std::chrono::system_clock::time_point::min();
    compensation_end_ = std::chrono::system_clock::time_point::min();
}

void StrictProcessUtilizationPartitionPolicy::Sched(const Status &status)
{
    const PID previous_head = run_queue_.empty() ? 0 : run_queue_.front();

    // delete all destroyed pids and their utilizations
    for (auto it = utils_.begin(); it != utils_.end();) {
        PID pid = it->first;
        if (status.process_status.find(pid) == status.process_status.end()) {
            it = utils_.erase(it);
        } else {
            ++it;
        }
    }

    // delete all destroyed processes and processes with no XQueues from the run queue
    for (auto it = run_queue_.begin(); it != run_queue_.end();) {
        PID pid = *it;
        auto ps_it = status.process_status.find(pid);
        if (ps_it == status.process_status.end() ||
           (ps_it->second->running_xqueues.empty() && ps_it->second->suspended_xqueues.empty())) {
            it = run_queue_.erase(it);
            pid_in_queue_.erase(pid);
        } else {
            ++it;
        }
    }

    // add new processes with XQueues to the run queue
    for (const auto &process : status.process_status) {
        PID pid = process.first;
        if (process.second->running_xqueues.empty() && process.second->suspended_xqueues.empty()) {
            continue;
        }
        if (pid_in_queue_.find(pid) != pid_in_queue_.end()) continue;
        run_queue_.emplace_back(pid);
        pid_in_queue_.insert(pid);
    }

    const bool head_changed = !run_queue_.empty() && run_queue_.front() != previous_head;
    RoundRobin(status, head_changed);
}

void StrictProcessUtilizationPartitionPolicy::RecvHint(std::shared_ptr<const Hint> hint)
{
    switch (hint->Type())
    {
    case kHintTypeUtilization:
    {
        auto h = std::dynamic_pointer_cast<const UtilizationHint>(hint);
        XASSERT(h != nullptr, "hint type not match");
        PID pid = h->Pid();
        if (pid == 0) break;
        Utilization util = h->Util();
        if (util < UTILIZATION_MIN || util > UTILIZATION_MAX) {
            XWARN("invalid utilization %d", util);
            break;
        }
        utils_[pid] = util;
        XINFO("utilization of process %u set to %d", pid, util);
        break;
    }
    case kHintTypeTimeslice:
    {
        auto h = std::dynamic_pointer_cast<const TimesliceHint>(hint);
        XASSERT(h != nullptr, "hint type not match");
        Timeslice ts_us = h->Ts();
        if (ts_us < TIMESLICE_MIN || ts_us > TIMESLICE_MAX) {
            XWARN("invalid timeslice %ld", ts_us);
            break;
        }
        timeslice_avg_us_ = ts_us;
        XINFO("timeslice set to %ld us", ts_us);
        break;
    }
    default:
        XWARN("unsupported hint type: %d", hint->Type());
        break;
    }
}

void StrictProcessUtilizationPartitionPolicy::RoundRobin(const Status &status, bool head_changed)
{
    std::list<XQueueHandle> xq_rq;
    std::list<XQueueHandle> suspend_xqs;

    // nothing to run
    if (run_queue_.empty()) {
        UpdateRunQueue(xq_rq);
        return;
    }

    // get the utilization of each process in the run_queue_
    int64_t total_util = 0;
    std::list<int64_t> util_list;
    for (const PID pid : run_queue_) {
        int64_t util = UTILIZATION_DEFAULT;
        auto it = utils_.find(pid);
        if (it != utils_.end()) util = it->second;
        total_util += util;
        util_list.push_back(util);
    }

    const auto now = system_clock::now();
    const bool rotate = !head_changed && timeslice_end_ <= now && compensation_started_
                     && compensation_end_ <= now;
    if (rotate) {
        run_queue_.splice(run_queue_.end(), run_queue_, run_queue_.begin());
        util_list.splice(util_list.end(), util_list, util_list.begin());
    }

    // A new head can take over immediately after the running process exits.
    const bool switched = head_changed || rotate;
    Timeslice run_ts;
    if (switched) {
        run_ts = timeslice_avg_us_ * static_cast<int64_t>(run_queue_.size())
               * util_list.front() / total_util;
        compensation_started_ = false;
        timeslice_end_ = now + microseconds(run_ts);
        AddTimer(timeslice_end_);
    } else if (timeslice_end_ > now) {
        run_ts = duration_cast<microseconds>(timeslice_end_ - now).count();
    } else {
        if (!compensation_started_) {
            const uint64_t overhead_us = GetOverheadUs(status, run_queue_.front());
            compensation_end_ = timeslice_end_ + microseconds(overhead_us);
            compensation_started_ = true;
            AddTimer(compensation_end_);
        }
        run_ts = duration_cast<microseconds>(compensation_end_ - now).count();
    }

    // the first pid in the run_queue_ is the running process
    // move all its XQueues to the xq_rq (XQueue Run Queue)
    PID run_pid = run_queue_.front();
    auto run_ps_it = status.process_status.find(run_pid);
    XASSERT(run_ps_it != status.process_status.end(), "process %d not found", run_pid);
    ProcessStatus &run_ps = *run_ps_it->second;
    XASSERT(!run_ps.running_xqueues.empty() || !run_ps.suspended_xqueues.empty(),
            "process %d has no XQueues", run_pid);
    xq_rq.insert(xq_rq.end(), run_ps.running_xqueues.begin(), run_ps.running_xqueues.end());
    xq_rq.insert(xq_rq.end(), run_ps.suspended_xqueues.begin(), run_ps.suspended_xqueues.end());

    // resume and set timeslice for the running XQueues
    for (const auto xq: xq_rq) {
        Resume(xq);
        SetTimeslice(xq, run_ts);
    }

    // set timeslice for the rest XQueues
    auto pid_it = std::next(run_queue_.begin());
    auto ut_it = std::next(util_list.begin());
    while (pid_it != run_queue_.end() && ut_it != util_list.end()) {
        PID pid = *pid_it;
        int64_t util = *ut_it;
        Timeslice ts = (int64_t)timeslice_avg_us_ * run_queue_.size() * util / total_util;
        const auto ps_it = status.process_status.find(pid);
        if (ps_it != status.process_status.end()) {
            for (XQueueHandle xq : ps_it->second->running_xqueues) {
                suspend_xqs.emplace_back(xq);
                SetTimeslice(xq, ts);
            }
            for (XQueueHandle xq : ps_it->second->suspended_xqueues) {
                suspend_xqs.emplace_back(xq);
                SetTimeslice(xq, ts);
            }
        }
        pid_it++;
        ut_it++;
    }

    // suspend the rest XQueues
    for (const auto xq: suspend_xqs) { Suspend(xq); }

    // if process has not been switched, no need to update run queue
    if (!switched) return;
    // update the run queue
    xq_rq.splice(xq_rq.end(), suspend_xqs);
    UpdateRunQueue(xq_rq);
}

uint64_t StrictProcessUtilizationPartitionPolicy::GetOverheadUs(const Status &status, PID pid)
{
    auto ps_it = status.process_status.find(pid);
    XASSERT(ps_it != status.process_status.end(), "process %d not found", pid);
    ProcessStatus &ps = *ps_it->second;
    // A process without running queues has no overhead to compensate.
    if (ps.running_xqueues.empty()) return 0;
    XQueueHandle xq = *ps.running_xqueues.begin();
    auto xq_it = status.xqueue_status.find(xq);
    XASSERT(xq_it != status.xqueue_status.end(), "XQueue 0x%lx not found", xq);
    return xq_it->second->sched_overhead;
}
