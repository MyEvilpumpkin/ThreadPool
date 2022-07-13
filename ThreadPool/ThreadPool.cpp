#include <iostream>
#include <queue>
#include <thread>
#include <chrono>
#include <mutex>
#include <future>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <any>
#include <atomic>
#include <variant>
#include <cassert>
#include <map>
#include <utility>
#include <functional>

class Task {
public:
    template <typename ReturnType, typename ...Types, typename ...Args>
    Task(ReturnType(*function)(Types...), Args&&... args) : isVoid{ std::is_void_v<ReturnType> } {
        if constexpr (std::is_void_v<ReturnType>) {
            voidFunction = std::bind(function, args...);
            anyFunction = []()->int { return 0; };
        }
        else {
            voidFunction = []()->void {};
            anyFunction = std::bind(function, args...);
        }
    }

    void operator() () {
        voidFunction();
        anyFunctionResult = anyFunction();
    }

    bool hasResult() {
        return !isVoid;
    }

    std::any getResult() const {
        assert(!isVoid);
        assert(anyFunctionResult.has_value());
        return anyFunctionResult;
    }

private:
    std::function<void()> voidFunction;
    std::function<std::any()> anyFunction;
    std::any anyFunctionResult;
    bool isVoid;
};

enum class TaskStatus {
    NEW,
    RUNNING,
    COMPLETED
};

struct TaskInfo {
    TaskStatus status = TaskStatus::NEW;
    std::any result;
};


class ThreadPool {
public:
    ThreadPool(const size_t nThreads) {
        threads.reserve(nThreads);
        for (size_t i = 0; i < nThreads; ++i) {
            threads.emplace_back(&ThreadPool::run, this);
        }
    }

    template <typename ReturnType, typename ...Types, typename ...Args>
    size_t addTask(ReturnType(*function)(Types...), Args&&... args) {
        const size_t taskId = newTaskId++;

        std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
        tasksInfo[taskId] = TaskInfo();
        tasksInfoLock.unlock();

        std::unique_lock<std::mutex> tasksLock(tasksMtx);
        tasks.emplace(Task(function, std::forward<Args>(args)...), taskId);
        tasksLock.unlock();
        tasksCv.notify_one();

        return taskId;
    }

    void wait(const size_t taskId) {
        std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
        tasksInfoCv.wait(tasksInfoLock, [this, taskId]()->bool {
            return taskId < newTaskId && tasksInfo[taskId].status == TaskStatus::COMPLETED;
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
        return taskId < newTaskId && tasksInfo[taskId].status == TaskStatus::RUNNING;
    }

    bool isCompleted(const size_t taskId) {
        std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
        return taskId < newTaskId && tasksInfo[taskId].status == TaskStatus::COMPLETED;
    }

    ~ThreadPool() {
        quite = true;
        tasksCv.notify_all();
        for (int i = 0; i < threads.size(); ++i) {
            threads[i].join();
        }
    }

private:

    void run() {
        while (!quite) {
            std::unique_lock<std::mutex> tasksLock(tasksMtx);
            tasksCv.wait(tasksLock, [this]()->bool { return !tasks.empty() || quite; });

            std::optional<std::pair<Task, size_t>> task;

            if (!tasks.empty() && !quite) {
                task = std::move(tasks.front());
                tasks.pop();
            }

            tasksLock.unlock();
            
            if (task) {

                task->first();

                std::unique_lock<std::mutex> tasksInfoLock(tasksInfoMtx);
                if (task->first.hasResult()) {
                    tasksInfo[task->second].result = task->first.getResult();
                }
                tasksInfo[task->second].status = TaskStatus::COMPLETED;
                ++nCompletedTasks;
                tasksInfoLock.unlock();

                tasksInfoCv.notify_all();
            }

        }
    }

    std::vector<std::thread> threads;

    std::queue<std::pair<Task, size_t>> tasks;
    std::mutex tasksMtx;
    std::condition_variable tasksCv;

    std::unordered_map<size_t, TaskInfo> tasksInfo;
    std::condition_variable tasksInfoCv;
    std::mutex tasksInfoMtx;

    std::atomic<bool> quite{ false };
    std::atomic<size_t> newTaskId{ 0 };
    std::atomic<size_t> nCompletedTasks{ 0 };
};

int int_sum(int a, int b) {
    return a + b;
}

void void_sum(int& c, int a, int b) {
    c = a + b;
}

void void_without_argument() {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    std::cout << "It's OK!" << std::endl;
}

int main() {
    ThreadPool t(3);
    int c;
    t.addTask(int_sum, 2, 3);               // id = 0
    t.addTask(void_sum, std::ref(c), 4, 6); // id = 1
    t.addTask(void_without_argument);       // id = 2

    {
        // variant 1
        int res;
        t.waitResult(0, res);
        std::cout << res << std::endl;

        // variant 2
        std::cout << std::any_cast<int>(t.waitResult(0)) << std::endl;
    }

    t.wait(1);
    std::cout << c << std::endl;

    t.waitAll(); // waiting for task with id 2

    std::cout << "All tasks completed..." << std::endl;

    return 0;
}

