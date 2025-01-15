#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib> // For std::size_t, std::atoi, etc.
#include <sstream> // For parsing core list
#include <immintrin.h> // For non-temporal memory instructions
#include <pthread.h> // For setting CPU affinity

/**
 * Pin the current thread to a specific CPU core.
 * @param coreId The ID of the CPU core to pin this thread to.
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
 * Perform non-temporal writes in a loop to generate memory traffic.
 * @param buffer        Pointer to allocated memory (aligned).
 * @param bufferSize    Size of the buffer in bytes.
 * @param durationSec   Duration to run in seconds.
 * @param bytesCounter  Atomic counter to accumulate the total bytes written.
 * @param coreId        CPU core to which the thread should be pinned.
 */
void thrashMemory(char* buffer, std::size_t bufferSize, double durationSec, 
                  std::atomic<uint64_t>& bytesCounter, int coreId) {
    // Pin the thread to the specified CPU core
    pinToCore(coreId);

    // The 128-bit value to be written non-temporally
    __m128i val = _mm_set1_epi8(0x5A); // arbitrary pattern: 0x5A

    // Record the start time
    auto startTime = std::chrono::steady_clock::now();
    auto endTime   = startTime + std::chrono::duration<double>(durationSec);

    // Non-temporal writes in a loop until time is up
    uint64_t localBytes = 0;
    while (true) {
        // Check time
        auto now = std::chrono::steady_clock::now();
        if (now >= endTime) {
            break;
        }

        // Write the buffer in 16-byte chunks
        for (std::size_t offset = 0; offset < bufferSize; offset += 16) {
            _mm_stream_si128(reinterpret_cast<__m128i*>(buffer + offset), val);
        }
        _mm_sfence(); // ensure ordering of non-temporal stores

        // Update localBytes (one pass is bufferSize bytes)
        localBytes += bufferSize;
    }

    // Accumulate to global atomic counter
    bytesCounter.fetch_add(localBytes, std::memory_order_relaxed);
}

/**
 * Parse a comma-separated list of CPU core IDs into a vector of integers.
 * @param coreListStr The string containing the core list (e.g., "11,12,13").
 * @return A vector of integers representing the parsed core IDs.
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

int main(int argc, char* argv[]) {
    // Defaults
    int numThreads      = 12;     // Number of threads to spawn
    double durationSec  = 5.0;    // Run thrashing for this many seconds
    std::size_t bufSizeMB = 256;  // Each thread's buffer size in MB
    std::string coreListStr = ""; // List of cores to pin threads to

    // Simple usage info
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        std::cout << "Usage: " << argv[0] 
                  << " [numThreads] [durationSec] [bufferSizeMB] [coreList]\n"
                  << "  numThreads   : number of threads (default 12)\n"
                  << "  durationSec  : time to run in seconds (default 5)\n"
                  << "  bufferSizeMB : buffer size per thread in MB (default 256)\n"
                  << "  coreList     : comma-separated list of cores to pin threads to (default: none)\n";
        return 0;
    }

    // Parse command line arguments
    if (argc > 1) numThreads     = std::atoi(argv[1]);
    if (argc > 2) durationSec    = std::atof(argv[2]);
    if (argc > 3) bufSizeMB      = std::strtoull(argv[3], nullptr, 10);
    if (argc > 4) coreListStr    = argv[4];

    std::vector<int> coreList = parseCoreList(coreListStr);

    std::cout << "Memory-thrashing microbenchmark\n";
    std::cout << "Threads          : " << numThreads << "\n";
    std::cout << "Duration         : " << durationSec << " s\n";
    std::cout << "Buffer Size/Thread: " << bufSizeMB << " MB\n";
    if (!coreList.empty()) {
        std::cout << "Core list         : " << coreListStr << "\n";
    } else {
        std::cout << "Threads are not pinned to any specific cores.\n";
    }
    std::cout << "\n";

    // Convert MB to bytes
    std::size_t bufSizeBytes = bufSizeMB * (1ULL << 20);

    // Prepare a large buffer for each thread (aligned)
    std::vector<char*> buffers(numThreads, nullptr);

    // Allocate and initialize buffers
    for (int i = 0; i < numThreads; ++i) {
        void* ptr = nullptr;
        if (posix_memalign(&ptr, 64, bufSizeBytes) != 0) {
            std::cerr << "Failed to allocate buffer for thread " << i << std::endl;
            return 1;
        }
        buffers[i] = static_cast<char*>(ptr);

        // Initialize to avoid soft page-faults later
        std::memset(buffers[i], 0, bufSizeBytes);
    }

    // Atomic counter for total bytes written
    std::atomic<uint64_t> totalBytes(0);

    // Launch threads
    std::vector<std::thread> workers;
    workers.reserve(numThreads);

    // Start time for the entire run (just for reference)
    auto globalStart = std::chrono::steady_clock::now();

    for (int t = 0; t < numThreads; ++t) {
        workers.emplace_back([&, t]() {
            // If coreList is not empty, distribute threads across specified cores
            int coreId = !coreList.empty() ? coreList[t % coreList.size()] : t % std::thread::hardware_concurrency();
            thrashMemory(buffers[t], bufSizeBytes, durationSec, totalBytes, coreId);
        });
    }

    // Wait for all threads to finish
    for (auto& th : workers) {
        th.join();
    }

    // End time
    auto globalEnd = std::chrono::steady_clock::now();
    double actualDuration = std::chrono::duration<double>(globalEnd - globalStart).count();

    // Calculate total memory traffic in GB
    double totalGB = static_cast<double>(totalBytes.load(std::memory_order_relaxed)) / (1ULL << 30);

    // Bandwidth = totalGB / time
    double bwGBs = totalGB / actualDuration;

    // Print results
    std::cout << "Total time          : " << actualDuration << " s (wall-clock)\n";
    std::cout << "Total data written  : " << totalGB << " GB\n";
    std::cout << "Average bandwidth   : " << bwGBs << " GB/s\n";

    // Free memory
    for (int i = 0; i < numThreads; ++i) {
        free(buffers[i]);
    }

    return 0;
}