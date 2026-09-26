# ParkSmart KE - build file (Linux, macOS, or Windows via MSYS2/MinGW or WSL)
#
#   make          build ./parksmart
#   make run      build and start the server (demo mode) on http://localhost:8080
#   make test     run the self-checks (fees, slots, plates)
#   make clean    remove the executables
#
# Requirements: a C++17 compiler and the SQLite3 development library
#   Ubuntu/Debian/WSL:  sudo apt install g++ make libsqlite3-dev
#   macOS (Homebrew):   brew install sqlite
#   Windows (MSYS2):    pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-sqlite3 make
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
LDLIBS   := -lsqlite3

# On Windows the socket library must be linked explicitly.
ifeq ($(OS),Windows_NT)
  LDLIBS += -lws2_32
  TARGET := parksmart.exe
else
  TARGET := parksmart
endif

SRC := src/main.cpp $(wildcard src/*.hpp)

$(TARGET): $(SRC) third_party/httplib.h
	$(CXX) $(CXXFLAGS) src/main.cpp -o $(TARGET) $(LDLIBS)

run: $(TARGET)
	./$(TARGET) --demo

# Self-checks for the fee, slot and plate logic (no server needed).
test: tests/test_core.cpp $(SRC)
	$(CXX) $(CXXFLAGS) tests/test_core.cpp -o test_core $(LDLIBS)
	./test_core

clean:
	rm -f $(TARGET) test_core test_core.exe

.PHONY: run test clean
