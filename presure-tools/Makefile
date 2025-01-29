# Makefile for memory streaming and bandwidth benchmarks

# Compiler
CXX = g++

# Compiler flags
CXXFLAGS = -O3 -march=native -std=c++17 -Wall -Wextra -pthread

# Target executable names
TARGET_MEM_STREAM = mem_stream
TARGET_MEM_BAND = mem_band
TARGET_CACHE_BAND = cacheb
TARGET_LLC_THRASH = llct

# Source files
SRC_MEM_STREAM = memory_stream.cpp
SRC_MEM_BAND = memory_band.cpp
SRC_CACHE_BAND = cache_band.cpp
SRC_LLC_THRASH = llc_thrash.cpp

# Build all targets
.PHONY: all
all: $(TARGET_MEM_STREAM) $(TARGET_MEM_BAND) $(TARGET_CACHE_BAND) $(TARGET_LLC_THRASH)

# Compile memory_stream target
$(TARGET_MEM_STREAM): $(SRC_MEM_STREAM)
	$(CXX) $(CXXFLAGS) -o $@ $<

# Compile memory_band target
$(TARGET_MEM_BAND): $(SRC_MEM_BAND)
	$(CXX) $(CXXFLAGS) -o $@ $<

# Compile cache_band target
$(TARGET_CACHE_BAND): $(SRC_CACHE_BAND)
	$(CXX) $(CXXFLAGS) -o $@ $<

# Compile llc_thrash target
$(TARGET_LLC_THRASH): $(SRC_LLC_THRASH)
	$(CXX) $(CXXFLAGS) -o $@ $<

# Alias for make memc (build mem_stream)
.PHONY: memc
memc: $(TARGET_MEM_STREAM)

# Alias for make memb (build mem_band)
.PHONY: memb
memb: $(TARGET_MEM_BAND)

# Alias for make cacheb (build cache_band)
.PHONY: cacheb
cacheb: $(TARGET_CACHE_BAND)

# Alias for make llct (build llc_thrash)
.PHONY: llct
llct: $(TARGET_LLC_THRASH)

# Clean the build files
.PHONY: clean
clean:
	rm -f $(TARGET_MEM_STREAM) $(TARGET_MEM_BAND) $(TARGET_CACHE_BAND) $(TARGET_LLC_THRASH)