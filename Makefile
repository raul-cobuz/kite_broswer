BUILD_DIR ?= build
RELEASE_DIR := $(BUILD_DIR)/release
DEBUG_DIR := $(BUILD_DIR)/debug
JOBS ?= $(shell nproc 2>/dev/null || echo 2)
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

.PHONY: all debug run test filters install clean

all:
	cmake -S . -B $(RELEASE_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(RELEASE_DIR) --target mybrowser --parallel $(JOBS)

debug:
	cmake -S . -B $(DEBUG_DIR) -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(DEBUG_DIR) --target mybrowser-debug --parallel $(JOBS)

test:
	cmake -S . -B $(RELEASE_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(RELEASE_DIR) --target test_url_utils test_settings test_history test_session test_adblock_rules test_profile test_omnibox test_sync --parallel $(JOBS)
	ctest --test-dir $(RELEASE_DIR) --output-on-failure

filters:
	python3 update_filters.py

run: all filters
	./mybrowser

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 mybrowser "$(DESTDIR)$(BINDIR)/kite"
	install -m 644 filters.json "$(DESTDIR)$(BINDIR)/filters.json"

clean:
	rm -rf $(BUILD_DIR) mybrowser mybrowser-debug
