CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -O2
PKGS     := gtk+-3.0 webkit2gtk-4.1
CFLAGS_PKG := $(shell pkg-config --cflags $(PKGS))
LIBS_PKG   := $(shell pkg-config --libs $(PKGS))

TARGET := mybrowser
SRC    := main.cpp

.PHONY: all clean run filters

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(CFLAGS_PKG) -o $@ $< $(LIBS_PKG)

filters:
	python3 update_filters.py

run: $(TARGET) filters
	./$(TARGET)

clean:
	rm -f $(TARGET)
