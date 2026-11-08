CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -O2
PKGS     := gtk+-3.0 webkit2gtk-4.1
CFLAGS_PKG := $(shell pkg-config --cflags $(PKGS))
LIBS_PKG   := $(shell pkg-config --libs $(PKGS))

TARGET       := mybrowser
DEBUG_TARGET := $(TARGET)-debug
SRC          := main.cpp
DEBUG_FLAGS  := -std=c++17 -Wall -Wextra -Wno-unused-parameter -g -O0

.PHONY: all clean run filters debug

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(CFLAGS_PKG) -o $@ $< $(LIBS_PKG)

debug:
	$(CXX) $(DEBUG_FLAGS) $(CFLAGS_PKG) -o $(DEBUG_TARGET) $(SRC) $(LIBS_PKG)

filters:
	python3 update_filters.py

run: $(TARGET) filters
	./$(TARGET)

clean:
	rm -f $(TARGET) $(DEBUG_TARGET)
