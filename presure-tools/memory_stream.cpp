#include <iostream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <immintrin.h>  // For non-temporal memory instructions
#include <unistd.h> 
#include <sched.h>      // For CPU affinity
#include <errno.h>
#include <cassert>

/**
 * Pin the current thread to a specific CPU core (optional).
 * @param cpu_core: The CPU core to bind the thread to.
 * @return 0 on success, -1 on failure.
 */
int pin_thread_to_core(int cpu_core) {
#if defined(__linux__)
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu_core, &mask);
    int result = sched_setaffinity(0, sizeof(mask), &mask);
    if (result != 0) {
        std::cerr << "Failed to pin thread to CPU " << cpu_core 
                  << ", error: " << strerror(errno) << std::endl;
        return -1;
    }
    return 0;
#else
    (void)cpu_core;
    return 0;
#endif
}

int main(int argc, char* argv[]) {
    /*
       Usage:
         ./mem_stream <memory_size_gb> <cpu_core> <iterations> <target_bandwidth> <chunk_count>
       Example:
         ./mem_stream 120 0 10 10 8
         write 120GB memory, bind to cpu 0, iteration 10 times, target bandwidth 10GB/s, 8 chunks for each iteration
    */

    size_t target_size_gb = 120;   // Default memory size (GB)
    int cpu_core          = -1;    // Default: no pin
    int iterations        = 10;    // Default number of outer iterations
    double target_bw_gbs  = 0.0;   // Default: no bandwidth limit
    int chunk_count       = 1;     // Default: do not split each iteration

    // Parse command-line arguments
    if (argc > 1) {
        target_size_gb = std::stoul(argv[1]);
    }
    if (argc > 2) {
        cpu_core = std::stoi(argv[2]);
    }
    if (argc > 3) {
        iterations = std::stoi(argv[3]);
    }
    if (argc > 4) {
        target_bw_gbs = std::stod(argv[4]);
    }
    if (argc > 5) {
        chunk_count = std::stoi(argv[5]);
    }

    // Pin the thread if requested
    if (cpu_core >= 0) {
        if (pin_thread_to_core(cpu_core) == 0) {
            std::cout << "Thread pinned to CPU core: " << cpu_core << std::endl;
        }
    }

    // Total size in bytes
    size_t total_size = static_cast<size_t>(target_size_gb) * (1ULL << 30);

    // Allocate memory
    char* buffer = reinterpret_cast<char*>(_mm_malloc(total_size, 64));
    if (!buffer) {
        std::cerr << "Failed to allocate " << target_size_gb << "GB memory!\n";
        return -1;
    }
    std::cout << "Allocated " << target_size_gb << " GB at " 
              << static_cast<void*>(buffer) << std::endl;

    // Pre-fill with zeros
    std::memset(buffer, 0, total_size);

    // Some logs
    std::cout << "Starting memory streaming test... (non-temporal stores)\n";
    std::cout << "Iterations      : " << iterations << "\n";
    std::cout << "Target BW       : " << (target_bw_gbs > 0 ? std::to_string(target_bw_gbs) + " GB/s" : "unlimited") << "\n";
    std::cout << "Chunk Count     : " << chunk_count << "\n\n";

    // Start time
    auto start_time = std::chrono::steady_clock::now();

    // Size per chunk
    // e.g. if total_size=120GB, chunk_count=8 => each chunk=15GB
    size_t chunk_size = total_size / chunk_count;
    double chunk_size_gb = static_cast<double>(chunk_size) / (1ULL << 30);

    for (int iter = 0; iter < iterations; ++iter) {
        // For each outer iteration, we now do 'chunk_count' sub-iterations
        // to write the entire buffer in smaller pieces
        for (int c = 0; c < chunk_count; ++c) {
            auto chunk_start_time = std::chrono::steady_clock::now();

            // Write chunk c [c*chunk_size, (c+1)*chunk_size)
            size_t offset_begin = c * chunk_size;
            size_t offset_end   = (c == chunk_count-1) ? total_size : (c+1)*chunk_size;

            // Prepare pattern
            __m128i val = _mm_set1_epi8(static_cast<char>(iter));

            for (size_t offset = offset_begin; offset < offset_end; offset += 16) {
                _mm_stream_si128(reinterpret_cast<__m128i*>(buffer + offset), val);
            }
            _mm_sfence();

            auto chunk_end_time = std::chrono::steady_clock::now();
            double chunk_elapsed = std::chrono::duration<double>(chunk_end_time - chunk_start_time).count();

            // current chunk real bandwidth
            double chunk_bw = chunk_size_gb / chunk_elapsed;  // GB/s

            // Throttle if needed
            if (target_bw_gbs > 0 && chunk_bw > target_bw_gbs) {
                // Desired time for chunk to not exceed target_bw
                double desired_time = chunk_size_gb / target_bw_gbs;
                double extra_sleep  = desired_time - chunk_elapsed;
                if (extra_sleep > 0) {
                    std::this_thread::sleep_for(std::chrono::duration<double>(extra_sleep));
                    // Recompute total chunk time (for logging if you want)
                    chunk_elapsed += extra_sleep;
                }
            }
        }

        // Print progress (just 1 line per iteration)
        std::cout << "\rIteration " << (iter+1) << "/" << iterations << " done." << std::flush;
    }

    // End time
    auto end_time = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();

    // Final stats
    double total_data_gb = static_cast<double>(target_size_gb) * iterations;
    double avg_bw = total_data_gb / elapsed_sec;

    std::cout << "\n\nMemory streaming completed.\n";
    std::cout << "Total data written: " << total_data_gb << " GB\n";
    std::cout << "Elapsed time      : " << elapsed_sec << " s\n";
    std::cout << "Average bandwidth : " << avg_bw << " GB/s\n";

    // Cleanup
    _mm_free(buffer);
    return 0;
}