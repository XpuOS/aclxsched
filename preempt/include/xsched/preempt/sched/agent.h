#pragma once

#include <memory>

#include "xsched/types.h"
#include "xsched/sched/protocol/hint.h"
#include "xsched/sched/protocol/event.h"
#include "xsched/sched/scheduler/scheduler.h"

namespace xsched::preempt
{

class SchedAgent
{
public:
    SchedAgent();
    ~SchedAgent();

    static void SendHint(std::shared_ptr<const sched::Hint> hint)
    {
        Instance().RelayHint(hint);
    }

    static void SendEvent(std::shared_ptr<const sched::Event> event)
    {
        Instance().RelayEvent(event);
    }

    static XResult SetScheduler(XSchedulerType scheduler, XPolicyType policy)
    {
        return Instance().ReplaceScheduler(scheduler, policy);
    }

private:
    void RelayHint(std::shared_ptr<const sched::Hint> hint);
    void RelayEvent(std::shared_ptr<const sched::Event> event);
    XResult ReplaceScheduler(XSchedulerType scheduler, XPolicyType policy);
    void InitScheduler(XSchedulerType scheduler, XPolicyType policy);
    void FiniScheduler();

    std::shared_ptr<sched::Scheduler> scheduler_ = nullptr;
    static SchedAgent &Instance();
};

} // namespace xsched::preempt
