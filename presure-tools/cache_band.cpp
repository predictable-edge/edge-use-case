#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <random>
#include <cmath>
#include <sched.h>       // for CPU affinity
#include <unistd.h>      // for sysconf
#include <errno.h>

/**
 * Pin the current thread to a specific CPU core (optional).
 * @param core_id The CPU core to which this thread should be pinned.
 * @return 0 on success, -1 on failure.
 */
int pinThreadToCore(int core_id) {
#if defined(__linux__)
    cpu_set_t cpuset; 
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int result = sched_setaffinity(0, sizeof(cpuset), &cpuset);
    if (result != 0) {
        std::cerr << "Failed to pin thread to CPU " << core_id
                  << ", error: " << strerror(errno) << std::endl;
        return -1;
    }
    return 0;
#else
    (void)core_id;
    return 0;
#endif
}

/**
 * A function that continuously "thrashes" an allocated memory buffer.
 * Now uses a "cumulative time window" approach to control bandwidth more smoothly.
 *
 * @param buffer         Pointer to the allocated memory for this thread.
 * @param sizeBytes      The size of this thread's buffer in bytes.
 * @param durationSec    The total duration (in seconds) to run thrashing.
 * @param targetBW       The target bandwidth in GB/s (0 = unlimited).
 * @param coreId         The CPU core to pin this thread to (-1 means no pin).
 * @param globalBytes    An atomic counter to track the total bytes written globally.
 */
void cacheThrash(
    char* buffer,
    size_t sizeBytes,
    double durationSec,
    double targetBW,     // GB/s
    int coreId,
    std::atomic<uint64_t>& globalBytes)
{
    // If coreId >= 0, pin the thread
    if (coreId >= 0) {
        pinThreadToCore(coreId);
    }

    // Start and end time for the entire run
    auto startTime = std::chrono::steady_clock::now();
    auto endTime   = startTime + std::chrono::duration<double>(durationSec);

    // Prepare random pattern generator
    unsigned seed = (unsigned)std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);

    // We'll accumulate iteration times in a "time window"
    double cumulativeTime = 0.0;      // total time spent in thrashing + sleeping
    uint64_t cumulativeBytes = 0ULL; // total bytes processed in this thread

    // Each iteration writes the entire 'sizeBytes' once
    const size_t stride = 16; // write chunk size (16 bytes)

    // Thrash loop
    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= endTime) {
            break;  // Time's up
        }

        auto iterationStart = std::chrono::high_resolution_clock::now();
        
        // 1) Perform memory writes
        int pattern = dist(rng);
        for (size_t offset = 0; offset < sizeBytes; offset += stride) {
            std::memset(buffer + offset, pattern, stride);
        }

        // 2) Measure this iteration's time
        auto iterationEnd = std::chrono::high_resolution_clock::now();
        double iterSec = std::chrono::duration<double>(iterationEnd - iterationStart).count();

        // 3) Update cumulative stats
        cumulativeTime   += iterSec;
        cumulativeBytes  += sizeBytes;

        // 4) Check if we exceed target bandwidth
        //    (bandwidth = totalGB / totalSec)
        if (targetBW > 0.0) {
            double totalGB = static_cast<double>(cumulativeBytes) / (1ULL << 30);
            double currentBW = totalGB / cumulativeTime; // GB/s

            if (currentBW > targetBW) {
                // We want: currentBW <= targetBW
                // => totalGB / (cumulativeTime + extraSleep) <= targetBW
                // => extraSleep >= totalGB / targetBW - cumulativeTime
                double desiredTime = totalGB / targetBW;
                double extraSleep  = desiredTime - cumulativeTime;
                if (extraSleep > 0.0) {
                    // Sleep to slow down
                    std::this_thread::sleep_for(std::chrono::duration<double>(extraSleep));
                    cumulativeTime += extraSleep;
                }
            }
        }
    }

    // After done, add to global counter
    globalBytes.fetch_add(cumulativeBytes, std::memory_order_relaxed);
}

int main(int argc, char* argv[])
{
    /**
     * Usage (example):
     *  ./cache_thrash [numThreads] [llcSizeMB] [durationSec] [targetBW] [coreList]
     *
     * Explanation:
     *  - numThreads : number of threads (default 12).
     *  - llcSizeMB  : total memory footprint (in MB) to thrash. Each thread gets 1/numThreads of that.
     *                 By default, you can approximate your LLC size or supply a custom value.
     *  - durationSec: how long (seconds) to run the thrash test (default 10s).
     *  - targetBW   : target bandwidth in GB/s per thread (0 = unlimited, default = 0).
     *  - coreList   : a comma-separated list of cores to pin each thread. 
     *                 If empty, do not pin. If fewer cores than threads, it cycles through them.
     */

    int numThreads = 12;         // default number of threads
    double llcSizeMB = 16 * 1024; // default 16GB for demonstration
    double durationSec = 10.0;
    double targetBW = 0.0;       // 0 = unlimited
    std::string coreListStr;     // if empty, no pinning

    if (argc > 1) {
        numThreads = std::atoi(argv[1]);
    }
    if (argc > 2) {
        llcSizeMB = std::stod(argv[2]);
    }
    if (argc > 3) {
        durationSec = std::stod(argv[3]);
    }
    if (argc > 4) {
        targetBW = std::stod(argv[4]);
    }
    if (argc > 5) {
        coreListStr = argv[5];
    }

    // Parse the core list into a vector<int>
    std::vector<int> coreList;
    if (!coreListStr.empty()) {
        size_t start = 0;
        while (true) {
            auto pos = coreListStr.find(',', start);
            if (pos == std::string::npos) {
                coreList.push_back(std::stoi(coreListStr.substr(start)));
                break;
            } else {
                coreList.push_back(std::stoi(coreListStr.substr(start, pos - start)));
                start = pos + 1;
            }
        }
    }

    std::cout << "Launching cache-thrashing microbenchmark:\n";
    std::cout << "  Number of threads : " << numThreads << "\n";
    std::cout << "  Total footprint   : " << llcSizeMB << " MB\n";
    std::cout << "  Duration          : " << durationSec << " s\n";
    std::cout << "  Target Bandwidth  : " 
              << (targetBW > 0 ? std::to_string(targetBW) + " GB/s" : "unlimited") << "\n";
    if (!coreList.empty()) {
        std::cout << "  Core list         : " << coreListStr << "\n";
    } else {
        std::cout << "  Core list         : none (no pin)\n";
    }

    // Convert MB to bytes
    double llcSizeBytes = llcSizeMB * 1024.0 * 1024.0;
    // Each thread gets an equal fraction of total footprint
    size_t perThreadBytes = static_cast<size_t>(llcSizeBytes / (double)numThreads);

    // We'll store pointers for each thread's memory chunk
    std::vector<char*> buffers(numThreads, nullptr);

    // Allocate for each thread
    for (int i = 0; i < numThreads; ++i) {
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 64, perThreadBytes) != 0) {
            std::cerr << "Failed to allocate memory for thread " << i << "\n";
            return 1;
        }
        buffers[i] = static_cast<char*>(ptr);
        // Initialize (to avoid lazy allocation/page faults on first access)
        std::memset(buffers[i], 0, perThreadBytes);
    }

    // Global counter to track total bytes processed
    std::atomic<uint64_t> globalBytes(0);

    // Launch threads
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    auto globalStart = std::chrono::steady_clock::now();

    for (int t = 0; t < numThreads; ++t) {
        // Decide which core to pin (if any)
        int coreId = -1; // means do not pin
        if (!coreList.empty()) {
            coreId = coreList[t % coreList.size()]; 
        }
        workers.emplace_back(
            cacheThrash,
            buffers[t],
            perThreadBytes,
            durationSec,
            targetBW,
            coreId,
            std::ref(globalBytes)
        );
    }

    // Wait for all threads to finish
    for (auto &th : workers) {
        th.join();
    }

    auto globalEnd = std::chrono::steady_clock::now();
    double totalElapsed = std::chrono::duration<double>(globalEnd - globalStart).count();

    // Calculate total data in GB
    double totalGB = static_cast<double>(globalBytes.load()) / (double)(1ULL << 30);
    double avgBW = totalGB / totalElapsed; // GB/s

    // Print summary
    std::cout << "\nAll threads finished.\n";
    std::cout << "  Elapsed time    : " << totalElapsed << " s\n";
    std::cout << "  Total data R/W  : " << totalGB << " GB\n";
    std::cout << "  Average BW      : " << avgBW << " GB/s\n";

    // Free memory
    for (int i = 0; i < numThreads; ++i) {
        free(buffers[i]);
    }

    return 0;
}