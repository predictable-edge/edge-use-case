#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <random>
#include <sched.h>
#include <unistd.h>
#include <x86intrin.h>
#include <immintrin.h>

/**
 * Get system's Last Level Cache (LLC) size in MB
 * @return LLC size in MB, defaults to 16MB if detection fails
 */
size_t getLLCSize() {
    // Try sysfs method first
    FILE* fp = fopen("/sys/devices/system/cpu/cpu0/cache/index3/size", "r");
    if (fp) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), fp)) {
            fclose(fp);
            // Remove newline if present
            char* nl = strchr(buffer, '\n');
            if (nl) *nl = '\0';
            
            // Get size and unit
            size_t size = 0;
            char unit = 'K';  // Default to KB
            if (sscanf(buffer, "%zu%c", &size, &unit) >= 1) {
                // Convert to MB based on unit
                switch(unit) {
                    case 'K': return size / 1024;        // KB to MB
                    case 'M': return size;               // Already MB
                    case 'G': return size * 1024;        // GB to MB
                    default:  return size / (1024*1024); // Assume bytes if no unit
                }
            }
        }
        fclose(fp);
    }

    // Fallback to lscpu method
    fp = popen("lscpu", "r");
    if (fp) {
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), fp)) {
            if (strstr(buffer, "L3 cache")) {
                // Match patterns like "18 MiB" or "18432 KiB"
                size_t size;
                char unit[4];
                if (sscanf(buffer, "%*[^:]: %zu %3s", &size, unit) == 2) {
                    pclose(fp);
                    if (strstr(unit, "Ki")) return size / 1024;      // KiB to MB
                    if (strstr(unit, "Mi")) return size;             // Already MB
                    if (strstr(unit, "Gi")) return size * 1024;      // GiB to MB
                }
            }
        }
        pclose(fp);
    }

    // Use default if detection fails
    std::cerr << "Warning: Could not detect LLC size, using default 16MB\n";
    return 16;
}


/**
 * Get cache line size in bytes
 * @return Cache line size, defaults to 64 bytes
 */
size_t getCacheLineSize() {
    size_t line_size = 64;
    FILE* fp = popen("getconf LEVEL1_DCACHE_LINESIZE", "r");
    if (fp) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), fp)) {
            line_size = std::stoul(buffer);
        }
        pclose(fp);
    }
    return line_size;
}

/**
 * Pin current thread to a specific CPU core
 * @param coreId CPU core to pin to
 * @return 0 on success, -1 on failure
 */
int pinToCore(int coreId) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(coreId, &cpuset);
    
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == -1) {
        std::cerr << "Failed to pin to core " << coreId << std::endl;
        return -1;
    }
    return 0;
}

/**
 * Generate cache set conflict pattern
 * @param setCount Number of cache sets
 * @param waysPerSet Number of ways per set
 * @return Vector of access offsets
 */
std::vector<size_t> generateConflictPattern(size_t setCount, size_t waysPerSet) {
    std::vector<size_t> pattern;
    pattern.reserve(setCount * waysPerSet);
    
    // Prime numbers for set traversal
    const size_t prime1 = 167;
    const size_t prime2 = 173;
    
    for (size_t set = 0; set < setCount; ++set) {
        size_t baseOffset = set * waysPerSet;
        for (size_t way = 0; way < waysPerSet; ++way) {
            size_t offset = (baseOffset + (way * prime1)) % (setCount * waysPerSet);
            pattern.push_back(offset);
        }
        // Add random jumps between sets
        pattern.push_back((baseOffset * prime2) % (setCount * waysPerSet));
    }
    
    return pattern;
}

/**
 * Perform LLC thrashing focused on cache pollution with minimal memory bandwidth
 * @param buffer Memory buffer to thrash
 * @param size Buffer size in bytes
 * @param duration Duration in seconds
 * @param coreId CPU core to pin to (-1 for no pinning)
 * @param conflicts Atomic counter for cache conflicts generated
 */
void llcThrash(char* buffer, 
               size_t size, 
               double duration, 
               int coreId,
               std::atomic<uint64_t>& conflicts) {
    if (coreId >= 0) {
        pinToCore(coreId);
    }

    const size_t cache_line_size = getCacheLineSize();
    const size_t sets_count = size / (cache_line_size * 16); // Assume 16-way set associative
    const size_t ways_per_set = 16;
    
    auto pattern = generateConflictPattern(sets_count, ways_per_set);
    
    // Create read and write patterns
    __m256i read_mask = _mm256_set1_epi64x(0x5555555555555555);
    __m256i write_mask = _mm256_set1_epi64x(0xAAAAAAAAAAAAAAAA);
    
    auto start_time = std::chrono::steady_clock::now();
    auto end_time = start_time + std::chrono::duration<double>(duration);
    
    uint64_t local_conflicts = 0;
    
    while (std::chrono::steady_clock::now() < end_time) {
        for (size_t offset : pattern) {
            char* addr = buffer + (offset * cache_line_size);
            
            // Read-modify-write to force cache coherency
            __m256i data = _mm256_load_si256(reinterpret_cast<__m256i*>(addr));
            data = _mm256_xor_si256(data, read_mask);
            _mm256_store_si256(reinterpret_cast<__m256i*>(addr), data);
            
            // Ensure cache line gets evicted
            _mm_clflush(addr);
            _mm_mfence();
            
            local_conflicts++;
        }
        
        // Short pause to reduce memory bandwidth
        _mm_pause();
    }
    
    conflicts.fetch_add(local_conflicts, std::memory_order_relaxed);
}

int main(int argc, char* argv[]) {
    // Default parameters
    int numThreads = 4;
    double duration = 10.0;
    std::string coreList = "";
    
    // Parse command line arguments
    if (argc > 1) numThreads = std::atoi(argv[1]);
    if (argc > 2) duration = std::atof(argv[2]);
    if (argc > 3) coreList = argv[3];
    
    // Parse core list
    std::vector<int> cores;
    if (!coreList.empty()) {
        size_t pos = 0;
        while ((pos = coreList.find(',')) != std::string::npos) {
            cores.push_back(std::stoi(coreList.substr(0, pos)));
            coreList.erase(0, pos + 1);
        }
        cores.push_back(std::stoi(coreList));
    }
    
    // Get LLC size and calculate buffer size (use actual LLC size)
    size_t llcSize = getLLCSize() * 1024 * 1024; // Convert MB to bytes
    size_t perThreadSize = llcSize / numThreads;
    
    std::cout << "LLC Disruption Configuration:\n"
              << "  Threads    : " << numThreads << "\n"
              << "  Duration   : " << duration << " seconds\n"
              << "  LLC Size   : " << (llcSize / (1024 * 1024)) << " MB\n"
              << "  Core List  : " << (cores.empty() ? "None" : argv[3]) << "\n\n";
    
    // Allocate memory for each thread
    std::vector<char*> buffers(numThreads);
    for (int i = 0; i < numThreads; ++i) {
        if (posix_memalign(reinterpret_cast<void**>(&buffers[i]), 64, perThreadSize) != 0) {
            std::cerr << "Memory allocation failed\n";
            return 1;
        }
        // Initialize memory with a pattern
        for (size_t j = 0; j < perThreadSize; j += 64) {
            _mm256_store_si256(
                reinterpret_cast<__m256i*>(buffers[i] + j),
                _mm256_set1_epi64x(j)
            );
        }
    }
    
    // Track cache conflicts
    std::atomic<uint64_t> totalConflicts{0};
    
    // Launch threads
    std::vector<std::thread> threads;
    auto startTime = std::chrono::steady_clock::now();
    
    for (int i = 0; i < numThreads; ++i) {
        int coreId = cores.empty() ? -1 : cores[i % cores.size()];
        threads.emplace_back(llcThrash, buffers[i], perThreadSize, 
                           duration, coreId, std::ref(totalConflicts));
    }
    
    // Wait for completion
    for (auto& thread : threads) {
        thread.join();
    }
    
    auto endTime = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(endTime - startTime).count();
    
    // Calculate and display statistics
    double conflictsPerSec = static_cast<double>(totalConflicts.load()) / elapsed;
    
    std::cout << "Results:\n"
              << "  Time             : " << elapsed << " seconds\n"
              << "  Cache Conflicts  : " << totalConflicts.load() << "\n"
              << "  Conflicts/Second : " << conflictsPerSec << "\n";
    
    // Cleanup
    for (auto buffer : buffers) {
        free(buffer);
    }
    
    return 0;
}