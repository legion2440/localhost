CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
TARGET := localhost_cpp

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean
