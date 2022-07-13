#include <iostream>
#include <chrono>

#include "ThreadPool.h"

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

    //t.waitAll(); // waiting for task with id 2

    std::cout << "All tasks completed..." << std::endl;

    return 0;
}

