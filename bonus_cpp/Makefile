CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror
TARGET := localhost_cpp
HEADERS := checked_syscalls.hpp config.hpp http.hpp http_utils.inc http_parser.inc response.inc \
	server.hpp server_io.inc server_routes.inc server_cgi.inc

all: $(TARGET)

$(TARGET): main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -include checked_syscalls.hpp main.cpp -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean
