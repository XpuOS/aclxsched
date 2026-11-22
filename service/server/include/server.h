#pragma once

#include <mutex>
#include <memory>
#include <thread>
#include <string>
#include <unordered_map>

#define CPPHTTPLIB_THREAD_POOL_COUNT 2
#include <httplib.h>
#include <json/json.h>
#include <ipc/ipc.h>

#include "xsched/types.h"
#include "xsched/utils/waitpid.h"
#include "xsched/sched/scheduler/local.h"
#include "xsched/sched/protocol/status.h"

namespace xsched::service
{

class Server
{
public:
    Server(const std::string &policy_name, const std::string &port);
    ~Server();

    void Run();
    void Stop();

private:
    struct ClientProcess
    {
        std::shared_ptr<ipc::Node> channel;
        uint32_t generation;
#if defined(__linux__)
        int message_queue_id = -1;
#endif
    };

    void RecvWorker();
    // Caller holds chan_mtx_. Keep the watch until the process exits.
    void CleanUpProcess(PID pid);
    void ProcessTerminate(PID pid, uint32_t generation);
    void SendHint(std::shared_ptr<const sched::Hint> hint);
    void Execute(std::shared_ptr<const sched::Operation> operation);

    XQueueHandle GetXQueueHandle(const Json::Value &request);
    bool GetXQueueStatus(XQueueHandle handle, sched::XQueueStatus &status);

    void GetXQueue(const httplib::Request &req, httplib::Response &res);
    void GetXQueues(const httplib::Request &req, httplib::Response &res);
    void PostXQueueConfig(const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader);
    void GetSchedulerPolicy(const httplib::Request &req, httplib::Response &res);
    void PostSchedulerPolicy(const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader);
    void PostHint(const httplib::Request &req, httplib::Response &res, const httplib::ContentReader &reader);

    std::mutex chan_mtx_;
    std::unique_ptr<ipc::Node> recv_chan_ = nullptr;
    std::unique_ptr<ipc::Node> self_chan_ = nullptr;
    std::unordered_map<PID, ClientProcess> clients_;
    uint32_t client_generation_ = 0;
    std::unique_ptr<utils::PidWaiter> pid_waiter_ = nullptr;
    std::unique_ptr<sched::LocalScheduler> scheduler_ = nullptr;
    
    uint16_t port_ = 0;
    Json::Reader json_reader_;
    Json::StreamWriterBuilder json_writer_;
    httplib::Server http_server_;
    std::unique_ptr<std::thread> http_thread_ = nullptr;
};

} // namespace xsched::service
