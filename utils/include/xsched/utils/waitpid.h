#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <utility>

#include "xsched/utils/common.h"

namespace xsched::utils
{

using TerminateCallback = std::function<void(PID, uint32_t)>;

class PidWaiter
{
public:
    PidWaiter() = default;
    virtual ~PidWaiter() = default;

    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual void AddWait(PID pid, uint32_t generation) = 0;

    static std::unique_ptr<PidWaiter> Create(TerminateCallback callback);
};

#if defined(__linux__)

class PidFdWaiter : public PidWaiter
{
public:
    PidFdWaiter(TerminateCallback callback): callback_(callback) {}
    ~PidFdWaiter() override;

    virtual void Start() override;
    virtual void Stop() override;
    virtual void AddWait(PID pid, uint32_t generation) override;
    static int OpenPidFd(PID pid, unsigned int flags);

private:
    struct Watch
    {
        int fd;
        uint32_t generation;
    };

    void WaitWorker();
    void RemovePidFd(int fd);

    int event_fd_ = -1;
    int epoll_fd_ = -1;

    std::mutex mtx_;
    std::unordered_map<PID, Watch> watches_;

    TerminateCallback callback_;
    std::unique_ptr<std::thread> thread_ = nullptr;
};

class ScanPidWaiter : public PidWaiter
{
public:
    ScanPidWaiter(TerminateCallback callback): running_(false), callback_(callback) {}
    ~ScanPidWaiter() override;
    
    virtual void Start() override;
    virtual void Stop() override;
    virtual void AddWait(PID pid, uint32_t generation) override;

private:
    void WaitWorker();

    std::mutex mtx_;
    std::atomic_bool running_;
    std::unordered_map<PID, uint32_t> pids_;
    TerminateCallback callback_;
    std::unique_ptr<std::thread> thread_ = nullptr;
};

// not working for /proc file system
class INotifyPidWaiter : public PidWaiter
{
public:
    INotifyPidWaiter(TerminateCallback callback): callback_(callback) {}
    ~INotifyPidWaiter() override;

    virtual void Start() override;
    virtual void Stop() override;
    virtual void AddWait(PID pid, uint32_t generation) override;

private:
    void WaitWorker();

    int inotify_fd_ = -1;
    std::mutex mtx_;
    std::unordered_map<int, std::pair<PID, uint32_t>> watches_;

    TerminateCallback callback_;
    std::unique_ptr<std::thread> thread_ = nullptr;
};

#elif defined(_WIN32)

class WinPidWaiter : public PidWaiter {
public:
    WinPidWaiter(TerminateCallback callback): callback_(callback) {}
    ~WinPidWaiter() override;

    virtual void Start() override;
    virtual void Stop() override;
    virtual void AddWait(PID pid, uint32_t generation) override;

private:
    void WaitWorker();

    TerminateCallback callback_;
    std::unordered_map<PID, uint32_t> pids_;
    std::mutex mtx_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};
};

#endif

} // namespace xsched::utils
