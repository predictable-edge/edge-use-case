# Compiler
CXX := g++

# Compiler Flags
CXXFLAGS := -std=c++17 -Wall -O3 $(shell pkg-config --cflags libavformat libavcodec libavutil libswscale)

# Linker Flags
LDFLAGS := $(shell pkg-config --libs libavformat libavcodec libavutil libswscale)

# Target Executable
TARGET := simple-transcode

# Source Files
SRCS := simple-transcode.cpp

RESULT := frame.log

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

# Phony Targets
.PHONY: all clean