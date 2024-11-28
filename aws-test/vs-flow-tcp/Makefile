# Makefile for compiling server.cpp and client.cpp

# Compiler
CXX = g++

# Compiler flags
CXXFLAGS = -std=c++11 -pthread -O2

# Target executable names
TARGETS = server client

# Source files
SRCS_server = server.cpp
SRCS_client = client.cpp

# Object files
OBJS_server = $(SRCS_server:.cpp=.o)
OBJS_client = $(SRCS_client:.cpp=.o)

# Default rule to build all targets
all: $(TARGETS)

# Rule to build the 'server' executable
server: $(OBJS_server)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Rule to build the 'client' executable
client: $(OBJS_client)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Generic rule to compile .cpp files into .o object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean rule to remove generated executables and object files
clean:
	rm -f $(TARGETS) $(OBJS_server) $(OBJS_client)

# Phony targets to prevent conflicts with files named 'all' or 'clean'
.PHONY: all clean
