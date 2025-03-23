# Compiler
CXX := g++

# Compiler Flags
CXXFLAGS := -std=c++17 -Wall -O3 -I/usr/local/ffmpeg/include

# Find OpenCV (without CUDA modules)
OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4)
OPENCV_LIBS := $(shell pkg-config --libs opencv4)

# CUDA paths
CUDA_CFLAGS := -I/usr/local/cuda/include
CUDA_LIBS := -L/usr/local/cuda/lib64 -lcudart

# Combined flags
CXXFLAGS += $(OPENCV_CFLAGS) $(CUDA_CFLAGS)

# Linker Flags
# Linker Flags
LDFLAGS := -L/usr/local/ffmpeg/lib -lavformat -lavcodec -lavutil -lswscale -lswresample -lzmq -lnvidia-ml $(OPENCV_LIBS) $(CUDA_LIBS)

# Target Executable
TARGET := detecting_tcp

# Source Files
SRCS := detecting_tcp.cpp

RESULT := frame*

# Object Files
OBJS := $(SRCS:.cpp=.o)

# Default Target
all: $(TARGET)

# Link the target executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Compile source files into object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean up build artifacts
clean:
	rm -f $(OBJS) $(TARGET) $(RESULT)

clog:
	rm -f $(RESULT)

# Phony Targets
.PHONY: all clean