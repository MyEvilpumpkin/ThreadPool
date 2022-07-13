#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <type_traits>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <unordered_map>
#include <any>
#include <optional>
#include <functional>

class ThreadPool {
public:
    ThreadPool(const size_t nThreads) : quite(false), newTaskId(0), nCompletedTasks(0) {
        threads.reserve(nThreads);
        for (size_t i = 0; i < nThreads; ++i) {
            threads.emplace_back(&ThreadPool::run, this);
        }
    }

    template <typename ReturnType, typename ...Types, typename ...Args>
    size_t addTask(ReturnType(*function)(Types...), Args&&... args) {
        std::unique_lock<std::mutex> tasksLock(tasksMtx);
        const size_t taskId = newTaskId++;
        tasks.emplace(taskId, function, std::forward<Args>(args)...);
        tasksLock.unlock();

        std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
        tasksInfo[taskId].status = TaskInfo::TaskStatus::NEW;
        tasksInfoLock.unlock();

        tasksCv.notify_one();

        return taskId;
    }

    void wait(const size_t taskId) {
        std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
        tasksInfoCv.wait(tasksInfoLock, [this, taskId]()->bool {
            return taskId < newTaskId&& tasksInfo[taskId].status == TaskInfo::TaskStatus::COMPLETED;
            });
    }

    std::any waitResult(const size_t taskId) {
        wait(taskId);
        return tasksInfo[taskId].result;
    }

    template<class T>
    void waitResult(const size_t taskId, T& value) {
        wait(taskId);
        value = std::any_cast<T>(tasksInfo[taskId].result);
    }

    void waitAll() {
        std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
        tasksInfoCv.wait(tasksInfoLock, [this]()->bool { return nCompletedTasks == newTaskId; });
    }

    bool isRunning(const size_t taskId) {
        std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
        return taskId < newTaskId&& tasksInfo[taskId].status == TaskInfo::TaskStatus::RUNNING;
    }

    bool isCompleted(const size_t taskId) {
        std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
        return taskId < newTaskId&& tasksInfo[taskId].status == TaskInfo::TaskStatus::COMPLETED;
    }

    ~ThreadPool() {
        quite = true;
        tasksCv.notify_all();
        for (size_t i = 0; i < threads.size(); ++i) {
            threads[i].join();
        }
    }

private:

    void run() {
        while (!quite) {
            std::unique_lock<std::mutex> tasksLock(tasksMtx);
            tasksCv.wait(tasksLock, [this]()->bool { return !tasks.empty() || quite; });

            std::optional<Task> optionalTask;

            if (!tasks.empty() && !quite) {
                optionalTask = std::move(tasks.front());
                tasks.pop();
            }

            tasksLock.unlock();

            if (optionalTask) {
                const Task& task = *optionalTask;
                const size_t taskId = task.id();

                std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
                tasksInfo[taskId].status = TaskInfo::TaskStatus::RUNNING;
                tasksInfoLock.unlock();

                const std::any& result = task();

                tasksInfoLock.lock();
                tasksInfo[taskId].result = result;
                tasksInfo[taskId].status = TaskInfo::TaskStatus::COMPLETED;
                ++nCompletedTasks;
                tasksInfoLock.unlock();

                tasksInfoCv.notify_all();
            }

        }
    }

    class Task {
    public:
        template <typename ReturnType, typename ...Types, typename ...Args>
        Task(const size_t id, ReturnType(*function)(Types...), Args&&... args) : _id(id) {
            if constexpr (std::is_void_v<ReturnType>) {
                const std::function<void()>& voidFunction = std::bind(function, args...);
                _function = [voidFunction]() {
                    voidFunction();
                    return std::any();
                };
            }
            else {
                _function = std::bind(function, args...);
            }
        }

        std::any operator() () const {
            return _function();
        }

        size_t id() const {
            return _id;
        }

    private:
        size_t _id;
        std::function<std::any()> _function;
    };

    struct TaskInfo {
        enum TaskStatus {
            NEW,
            RUNNING,
            COMPLETED
        } status;
        std::any result;
    };

    std::vector<std::thread> threads;

    std::queue<Task> tasks;
    std::condition_variable tasksCv;
    std::mutex tasksMtx;

    std::unordered_map<size_t, TaskInfo> tasksInfo;
    std::condition_variable tasksInfoCv;
    std::mutex tasksInfoMtx;

    std::atomic<bool> quite;
    std::atomic<size_t> newTaskId;
    std::atomic<size_t> nCompletedTasks;
};

#endif // !THREAD_POOL_H
