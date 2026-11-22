#include <string>
#include <signal.h>

#if defined(__linux__)
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/inotify.h>
#elif defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "xsched/utils/log.h"
#include "xsched/utils/common.h"
#include "xsched/utils/xassert.h"
#include "xsched/utils/waitpid.h"

#define SCAN_INTERVAL_US 1000

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

using namespace xsched::utils;

std::unique_ptr<PidWaiter> PidWaiter::Create(TerminateCallback callback)
{
    if (callback == nullptr) XERRO("callback is nullptr");

#if defined(__linux__)
    // check if the system supports pidfd_open
    int self_pid_fd = PidFdWaiter::OpenPidFd(GetProcessId(), 0);
    if (self_pid_fd == -1) {
        XWARN("pidfd_open is not supported, using scan method, which may consume more CPU");
        return std::make_unique<ScanPidWaiter>(callback);
    }

    XASSERT(!close(self_pid_fd), "fail to close self pid fd");
    XINFO("pidfd_open is supported, using pidfd_wait method");
    return std::make_unique<PidFdWaiter>(callback);
#elif defined(_WIN32)
    return std::make_unique<WinPidWaiter>(callback);
#endif
}

#if defined(__linux__)

PidFdWaiter::~PidFdWaiter()
{
    this->Stop();
}

void PidFdWaiter::Start()
{
    event_fd_ = eventfd(0, EFD_CLOEXEC);
    XASSERT(event_fd_ >= 0, "fail to create event fd");
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    XASSERT(epoll_fd_ >= 0, "fail to create epoll fd");

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = 0;
    XASSERT(!epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev),
            "fail to add event fd to epoll");

    thread_ = std::make_unique<std::thread>(&PidFdWaiter::WaitWorker, this);
}

void PidFdWaiter::Stop()
{
    if (thread_ != nullptr) {
        XASSERT(!eventfd_write(event_fd_, 1), "fail to write event fd");
        thread_->join();
        thread_ = nullptr;
    }

    if (event_fd_ >= 0) close(event_fd_);
    if (epoll_fd_ >= 0) close(epoll_fd_);
    for (const auto &entry : watches_) close(entry.second.fd);
    
    event_fd_ = -1;
    epoll_fd_ = -1;
    watches_.clear();
}

void PidFdWaiter::AddWait(PID pid, uint32_t generation)
{
    XASSERT(pid > 0 && generation != 0, "invalid pidfd watch");
    const int pid_fd = OpenPidFd(pid, 0);
    if (pid_fd < 0 && errno != ESRCH) XERRO("fail to open pid fd for pid %d", pid);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (pid_fd >= 0) {
            struct epoll_event ev{};
            ev.events = EPOLLIN;
            // Zero is reserved for the event that stops the worker.
            ev.data.u64 = (static_cast<uint64_t>(generation) << 32) | static_cast<uint32_t>(pid);
            XASSERT(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, pid_fd, &ev) == 0,
                    "fail to add pid fd to epoll for pid %d", pid);
        }

        auto previous = watches_.find(pid);
        if (previous != watches_.end()) {
            RemovePidFd(previous->second.fd);
            watches_.erase(previous);
        }
        if (pid_fd >= 0) watches_.emplace(pid, Watch{pid_fd, generation});
    }

    if (pid_fd < 0) callback_(pid, generation);
}

void PidFdWaiter::WaitWorker()
{
    struct epoll_event ev;
    while (true) {
        if (epoll_wait(epoll_fd_, &ev, 1, -1) == -1) {
            XASSERT(errno == EINTR, "fail during epoll wait");
            continue;
        }

        if (ev.data.u64 == 0) {
            eventfd_t v;
            XASSERT(!eventfd_read(event_fd_, &v), "fail to read event fd");
            return;
        }

        const auto pid = static_cast<PID>(static_cast<uint32_t>(ev.data.u64));
        const auto generation = static_cast<uint32_t>(ev.data.u64 >> 32);
        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto it = watches_.find(pid);
            if (it == watches_.end() || it->second.generation != generation) continue;
            RemovePidFd(it->second.fd);
            watches_.erase(it);
        }
        callback_(pid, generation);
    }
}

void PidFdWaiter::RemovePidFd(int fd)
{
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) != 0 && errno != ENOENT) {
        XWARN("fail to remove pid fd %d from epoll, errno: %d", fd, errno);
    }
    if (close(fd) != 0) XWARN("fail to close pid fd %d, errno: %d", fd, errno);
}

int PidFdWaiter::OpenPidFd(PID pid, unsigned int flags)
{
	return syscall(SYS_pidfd_open, (pid_t)pid, flags);
}

ScanPidWaiter::~ScanPidWaiter()
{
    this->Stop();
}

void ScanPidWaiter::Start()
{
    running_.store(true);
    thread_ = std::make_unique<std::thread>(&ScanPidWaiter::WaitWorker, this);
}

void ScanPidWaiter::Stop()
{
    running_.store(false);
    if (thread_ != nullptr) thread_->join();
    thread_ = nullptr;
}

void ScanPidWaiter::AddWait(PID pid, uint32_t generation)
{
    std::lock_guard<std::mutex> lock(mtx_);
    pids_[pid] = generation;
}

void ScanPidWaiter::WaitWorker()
{
    while (running_.load()) {
        std::unordered_map<PID, uint32_t> current_pids;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            current_pids = pids_;
        }

        for (const auto &[pid, generation] : current_pids) {
            if (kill(pid, 0) == 0) continue;
            if (errno != ESRCH) {
                XWARN("fail to test process %d, errno: %d", pid, errno);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mtx_);
                auto it = pids_.find(pid);
                if (it == pids_.end() || it->second != generation) continue;
                pids_.erase(it);
            }
            callback_(pid, generation);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(SCAN_INTERVAL_US));
    }
}

INotifyPidWaiter::~INotifyPidWaiter()
{
    this->Stop();
}

void INotifyPidWaiter::Start()
{
    inotify_fd_ = inotify_init1(0);
    XASSERT(inotify_fd_ >= 0, "fail to create inotify fd");
    thread_ = std::make_unique<std::thread>(&INotifyPidWaiter::WaitWorker, this);
}

void INotifyPidWaiter::Stop()
{
    if (inotify_fd_ >= 0) close(inotify_fd_);
    inotify_fd_ = -1;

    if (thread_ != nullptr) thread_->join();
    thread_ = nullptr;
}

void INotifyPidWaiter::AddWait(PID pid, uint32_t generation)
{
    std::string proc_path = "/proc/" + std::to_string(pid);
    std::unique_lock<std::mutex> lock(mtx_);
    int wd = inotify_add_watch(inotify_fd_, proc_path.c_str(), IN_DELETE_SELF);
    if (wd < 0) {
        // check if the process is already terminated
        if (errno == ENOENT) {
            XDEBG("process %d is already terminated", pid);
            lock.unlock();
            callback_(pid, generation);
            return;
        }
        XERRO("fail to add watch for pid %d", pid);
    }
    watches_[wd] = {pid, generation};
}

void INotifyPidWaiter::WaitWorker()
{
    char buf[4096];
    while (inotify_fd_ >= 0) {
        ssize_t n = read(inotify_fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EBADF) return; // inotify fd is closed
            XWARN("read error during inotify wait");
            continue;
        }

        ssize_t i = 0;
        while (i < n) {
            struct inotify_event *event = (struct inotify_event *) &buf[i];
            i += sizeof(struct inotify_event) + event->len;
            if (event->mask & IN_DELETE_SELF) {
                std::unique_lock<std::mutex> lock(mtx_);
                auto it = watches_.find(event->wd);
                if (it == watches_.end()) continue;
                const auto [pid, generation] = it->second;
                watches_.erase(it);
                inotify_rm_watch(inotify_fd_, event->wd);
                lock.unlock();
                callback_(pid, generation);
            }
        }
    }
}

#elif defined(_WIN32)

WinPidWaiter::~WinPidWaiter()
{
    this->Stop();
}

void WinPidWaiter::Start()
{
    running_.store(true);
    thread_ = std::make_unique<std::thread>(&WinPidWaiter::WaitWorker, this);
}

void WinPidWaiter::Stop()
{
    running_.store(false);
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    thread_ = nullptr;
}

void WinPidWaiter::AddWait(PID pid, uint32_t generation)
{
    std::lock_guard<std::mutex> lock(mtx_);
    pids_[pid] = generation;
}

void WinPidWaiter::WaitWorker()
{
    while (running_.load()) {
        std::unordered_map<PID, uint32_t> current_pids;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            current_pids = pids_;
        }

        for (const auto &[pid, generation] : current_pids) {
            HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
            if (process == NULL) {
                const DWORD error = GetLastError();
                if (error != ERROR_INVALID_PARAMETER) {
                    XWARN("fail to open process " FMT_PID ", error: %lu", pid, error);
                    continue;
                }
            } else {
                DWORD exit_code;
                const bool exited = GetExitCodeProcess(process, &exit_code)
                                  && exit_code != STILL_ACTIVE;
                CloseHandle(process);
                if (!exited) continue;
            }

            {
                std::lock_guard<std::mutex> lock(mtx_);
                auto it = pids_.find(pid);
                if (it == pids_.end() || it->second != generation) continue;
                pids_.erase(it);
            }
            callback_(pid, generation);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(SCAN_INTERVAL_US));
    }
}

#endif
