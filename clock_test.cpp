#define _POSIX_C_SOURCE 199309L

#include <iostream>
#include <ctime>
#include <thread>
#include <chrono>

int main()
{
    struct timespec start;
    struct timespec end;

    clock_gettime(CLOCK_MONOTONIC, &start);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    clock_gettime(CLOCK_MONOTONIC, &end);

    long long start_ns =
        start.tv_sec * 1000000000LL + start.tv_nsec;

    long long end_ns =
        end.tv_sec * 1000000000LL + end.tv_nsec;

    long long elapsed_ns = end_ns - start_ns;

    std::cout << "Elapsed: "
              << elapsed_ns
              << " ns\n";

    return 0;
}