#include <iostream>
#include <chrono>

#include "ThreadPool.h"

int intSum(int a, int b) {
    return a + b;
}

void voidSum(int& c, int a, int b) {
    c = a + b;
}

void voidWithoutArgument() {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    std::cout << "It's OK!" << std::endl;
}

int main() {
    mep::ThreadPool threadPool;
	threadPool.start(3);

    int task1Result;

	// std::bind
    mep::ThreadPool::TaskId task0 = threadPool.addTask(std::bind(intSum, 2, 3));
	// lambda
	mep::ThreadPool::TaskId task1 = threadPool.addTask([&task1Result]() { voidSum(task1Result, 4, 6); });
	// function
	mep::ThreadPool::TaskId task2 = threadPool.addTask(voidWithoutArgument);

    {
        mep::ThreadPool::WaitingResult waitingResult = threadPool.waitResult(task0);
        std::cout << std::any_cast<int>(waitingResult.taskInfo->result) << std::endl;
    }

	{
		threadPool.wait(1); // Using a raw int instead of a TaskId is not recommended
		std::cout << task1Result << std::endl;
	}


    threadPool.waitAll(); // waiting for task2

    std::cout << "All tasks completed..." << std::endl;

    return 0;
}

