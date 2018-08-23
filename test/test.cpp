#include <iostream>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

void foo()
{
    std::this_thread::sleep_for(2s);
}

int main()
{
    std::cout << "Hello waiter" << std::endl; // flush is intentional
    std::cout << "Wait for main" << std::endl;
    std::this_thread::sleep_for(10s);
    std::cout << "start foo" << std::endl;
    foo();
    std::cout << "end programm" << std::endl;
}

