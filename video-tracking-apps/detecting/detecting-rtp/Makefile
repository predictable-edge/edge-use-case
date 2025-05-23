# Compiler
CXX := g++

# Compiler Flags
CXXFLAGS := -std=c++17 -Wall -O3 -I/usr/local/ffmpeg/include

# Linker Flags
LDFLAGS := -L/usr/local/ffmpeg/lib -lavformat -lavcodec -lavutil -lswscale -lswresample -lzmq

# Target Executable
TARGET := detecting_rtp

# Source Files
SRCS := detecting_rtp.cpp

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