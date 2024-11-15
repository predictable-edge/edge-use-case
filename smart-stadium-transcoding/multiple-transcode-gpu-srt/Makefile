# Compiler
CXX := g++

CFLAGS += $(shell pkg-config --cflags libavcodec libavformat libavutil libavfilter libswscale libavdevice)
LDFLAGS += $(shell pkg-config --libs libavcodec libavformat libavutil libavfilter libswscale libavdevice)

# Target Executable
TARGET := multiple-transcode-gpu

# Source Files
SRCS := multiple-transcode-gpu.cpp

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