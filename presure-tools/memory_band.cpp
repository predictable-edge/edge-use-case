#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <immintrin.h>
#include <pthread.h>
#include <iomanip>
#include <algorithm>
#include <cmath>

// If your system supports NUMA, you can uncomment these headers
// #include <numa.h>
// #include <numaif.h>

/**
 * Pin the current thread to a specific CPU core.
 * @param coreId The ID of the CPU core to pin this thread to
 */
void pinToCore(int coreId) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (rc != 0) {
        std::cerr << "Error calling pthread_setaffinity_np: " << strerror(rc) << "\n";
    } else {
        std::cout << "Thread pinned to CPU core: " << coreId << "\n";
    }
}

/**
 * Parse a comma-separated list of CPU core IDs
 * @param coreListStr String containing comma-separated core IDs
 * @return Vector of core IDs
 */
std::vector<int> parseCoreList(const std::string& coreListStr) {
    std::vector<int> coreList;
    std::stringstream ss(coreListStr);
    std::string coreId;
    
    while (std::getline(ss, coreId, ',')) {
        coreList.push_back(std::stoi(coreId));
    }
    return coreList;
}

/**
 * Perform controlled non-temporal writes to generate specific memory traffic
 * while attempting to bypass caches as much as possible.
 *
 * @param buffer Pointer to allocated memory (aligned)
 * @param bufferSize Size of the buffer in bytes
 * @param durationSec Duration to run in seconds
 * @param bytesCounter Atomic counter to accumulate total bytes written
 * @param coreId CPU core to pin the thread to
 * @param targetBandwidthGBs Target bandwidth in GB/s (0 means maximum bandwidth)
 * @param doFlush Whether to flush cache lines after writing (reduces LLC pollution, but lowers write speed)
 * @param warmupSec Optional warm-up time in seconds, to let the system reach a steady state
 */
void thrashMemory(char* buffer, 
                  std::size_t bufferSize, 
                  double durationSec,
                  std::atomic<uint64_t>& bytesCounter, 
                  int coreId, 
                  double targetBandwidthGBs,
                  bool doFlush = false,
                  double warmupSec = 0.0) 
{
    // Pin thread to specified CPU core
    pinToCore(coreId);

    // Optional: if you want to bind memory to the same NUMA node as the CPU core,
    // you could do something like numa_alloc_onnode(...) or mbind(...).
    // This is commented out for portability:
    // int nodeId = /* determine node based on coreId if needed */;
    // numa_alloc_onnode(bufferSize, nodeId);

    // The 128-bit value to write non-temporally
    __m128i val = _mm_set1_epi8(0x5A);

    // Calculate end time for the real test
    auto startTime = std::chrono::steady_clock::now();
    auto endTime   = startTime + std::chrono::duration<double>(durationSec);

    // Warm-up phase (optional)
    if (warmupSec > 0.0) {
        auto warmupEnd = startTime + std::chrono::duration<double>(warmupSec);
        while (std::chrono::steady_clock::now() < warmupEnd) {
            for (std::size_t offset = 0; offset < bufferSize; offset += 16) {
                _mm_stream_si128(reinterpret_cast<__m128i*>(buffer + offset), val);
            }
            _mm_sfence();
        }
        // Update startTime and endTime after warm-up
        startTime = std::chrono::steady_clock::now();
        endTime   = startTime + std::chrono::duration<double>(durationSec);
    }

    uint64_t localBytes = 0;

    // If user wants to control bandwidth, we use an absolute timeline approach
    // This approach attempts to start each "bulk write" at a precise time, 
    // reducing error accumulation from repeated sleeps.
    const double BYTES_PER_GB = 1e9;
    double writesPerSecond = 0;
    if (targetBandwidthGBs > 0) {
        writesPerSecond = (targetBandwidthGBs * BYTES_PER_GB) / static_cast<double>(bufferSize);
    }

    // Next "deadline" for writing, set to now
    auto nextWriteDeadline = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < endTime) {
        // Perform the write in 16-byte chunks
        for (std::size_t offset = 0; offset < bufferSize; offset += 16) {
            _mm_stream_si128(reinterpret_cast<__m128i*>(buffer + offset), val);

            // Optionally flush the cache line to further reduce LLC residency
            // This can degrade performance significantly, 
            // so only do it if your goal is to truly bypass caches.
            if (doFlush) {
                _mm_clflushopt(reinterpret_cast<void*>(buffer + offset));
            }
        }
        _mm_sfence();

        localBytes += bufferSize;

        // If we have a target bandwidth, we check how long the operation took 
        // and sleep accordingly until next "deadline"
        if (writesPerSecond > 0) {
            // Convert double seconds to steady_clock::duration
            auto increment = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / writesPerSecond)
            );
            nextWriteDeadline += increment;

            auto now = std::chrono::steady_clock::now();
            if (now < nextWriteDeadline) {
                std::this_thread::sleep_until(nextWriteDeadline);
            } else {
                // If we are behind schedule, just continue without sleeping
            }
        }
    }

    // Accumulate total bytes
    bytesCounter.fetch_add(localBytes, std::memory_order_relaxed);
}

int main(int argc, char* argv[]) {
    // Default parameters
    int numThreads = 2;            // Example
    double durationSec = 5.0;      // Test duration
    std::size_t bufSizeMB = 256;   // Per-thread buffer size in MB
    double targetBandwidthGBs = 0; // 0 => max bandwidth
    std::string coreListStr;
    double warmupSec = 0.0;        // Optional warm-up period
    bool doFlush = false;          // Whether to flush cache lines after each write

    // Simple usage
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        std::cout << "Usage: " << argv[0] 
                  << " [numThreads] [durationSec] [bufferSizeMB] [targetBandwidthGBs] [coreList] [warmupSec] [flush]\n"
                  << "  numThreads        : Number of threads (default 2)\n"
                  << "  durationSec       : Test duration in seconds (default 5)\n"
                  << "  bufferSizeMB      : Per-thread buffer size in MB (default 256)\n"
                  << "  targetBandwidthGBs: Target bandwidth in GB/s (0 for max, default 0)\n"
                  << "  coreList          : Comma-separated list of cores (optional)\n"
                  << "  warmupSec         : Warm-up time in seconds before the main test (default 0)\n"
                  << "  flush             : Whether to flush cache lines (0 or 1, default 0)\n";
        return 0;
    }

    // Parse command line arguments
    if (argc > 1) numThreads = std::atoi(argv[1]);
    if (argc > 2) durationSec = std::atof(argv[2]);
    if (argc > 3) bufSizeMB = std::strtoull(argv[3], nullptr, 10);
    if (argc > 4) targetBandwidthGBs = std::atof(argv[4]);
    if (argc > 5) coreListStr = argv[5];
    if (argc > 6) warmupSec = std::atof(argv[6]);
    if (argc > 7) doFlush = (std::atoi(argv[7]) != 0);

    // Parse core list
    std::vector<int> coreList = parseCoreList(coreListStr);

    // Print configuration
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Memory-thrashing microbenchmark (improved version)\n"
              << "Threads            : " << numThreads << "\n"
              << "Duration           : " << durationSec << " s\n"
              << "Buffer Size/Thread : " << bufSizeMB << " MB\n"
              << "Target Bandwidth   : " 
              << (targetBandwidthGBs > 0 ? std::to_string(targetBandwidthGBs) + " GB/s" 
                                         : "Maximum") << "\n"
              << "Warm-up Time       : " << warmupSec << " s\n"
              << "Flush Cache Lines  : " << (doFlush ? "Yes" : "No") << "\n";

    if (!coreList.empty()) {
        std::cout << "Core list          : " << coreListStr << "\n";
    } else {
        std::cout << "Threads are not pinned to a custom core list.\n";
    }
    std::cout << std::endl;

    // Convert MB to bytes
    std::size_t bufSizeBytes = bufSizeMB * (1ULL << 20);

    // Allocate aligned buffers for each thread
    std::vector<char*> buffers(numThreads, nullptr);
    for (int i = 0; i < numThreads; ++i) {
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 64, bufSizeBytes) != 0) {
            std::cerr << "Failed to allocate buffer for thread " << i << std::endl;
            return 1;
        }
        buffers[i] = static_cast<char*>(ptr);
        // Initialize memory to ensure pages are physically mapped
        std::memset(buffers[i], 0, bufSizeBytes);
    }

    // Atomic counter for total bytes written
    std::atomic<uint64_t> totalBytes(0);

    // Launch worker threads
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    auto globalStart = std::chrono::steady_clock::now();

    // Distribute target bandwidth across threads if specified
    double perThreadBandwidthGBs = (targetBandwidthGBs > 0) 
                                 ? (targetBandwidthGBs / numThreads) 
                                 : 0;

    for (int t = 0; t < numThreads; ++t) {
        workers.emplace_back([&, t]() {
            // If user didn't provide a core list, we fallback to 
            // (t % hardware_concurrency) or any other logic
            int assignedCore = !coreList.empty() 
                               ? coreList[t % coreList.size()] 
                               : (t % std::thread::hardware_concurrency());

            thrashMemory(
                buffers[t], 
                bufSizeBytes, 
                durationSec, 
                totalBytes, 
                assignedCore, 
                perThreadBandwidthGBs,
                doFlush,
                warmupSec
            );
        });
    }

    // Wait for all threads to finish
    for (auto& th : workers) {
        th.join();
    }

    auto globalEnd = std::chrono::steady_clock::now();
    double actualDuration = std::chrono::duration<double>(globalEnd - globalStart).count();

    // Calculate and display results
    double totalGB = static_cast<double>(totalBytes.load(std::memory_order_relaxed)) / (1ULL << 30);
    double achievedBwGBs = totalGB / actualDuration;

    std::cout << "Results:\n"
              << "Total time          : " << actualDuration << " s (wall-clock)\n"
              << "Total data written  : " << std::fixed << std::setprecision(2) << totalGB << " GB\n"
              << "Achieved bandwidth  : " << achievedBwGBs << " GB/s\n";

    if (targetBandwidthGBs > 0) {
        double accuracy = (achievedBwGBs / targetBandwidthGBs) * 100.0;
        std::cout << "Target accuracy     : " << accuracy << "%\n";
    }

    // Cleanup
    for (int i = 0; i < numThreads; ++i) {
        free(buffers[i]);
    }

    return 0;
}