# Makefile for compiling client.cpp

# Compiler
CXX = g++

# Compiler flags
CXXFLAGS = -std=c++11 -pthread -O2

# Target executable name
TARGET = client

# Source files
SRCS = client.cpp

# Object files
OBJS = $(SRCS:.cpp=.o)

# Default rule to build the target
all: $(TARGET)

# Rule to build the target executable
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)

# Rule to compile source files into object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean up generated files
clean:
	rm -f $(TARGET) $(OBJS)

# Phony targets
.PHONY: all clean