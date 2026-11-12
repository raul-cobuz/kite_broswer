BUILD_DIR ?= build
RELEASE_DIR := $(BUILD_DIR)/release
DEBUG_DIR := $(BUILD_DIR)/debug
JOBS ?= $(shell nproc 2>/dev/null || echo 2)
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
APPDIR ?= $(DATADIR)/applications
ICONDIR ?= $(DATADIR)/icons/hicolor/scalable/apps

.PHONY: all debug run test filters install default-browser clean

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
	install -d "$(DESTDIR)$(APPDIR)"
	sed 's|@EXEC@|$(BINDIR)/kite|g' data/kite.desktop.in > "$(DESTDIR)$(APPDIR)/kite.desktop"
	chmod 644 "$(DESTDIR)$(APPDIR)/kite.desktop"
	install -d "$(DESTDIR)$(ICONDIR)"
	install -m 644 data/kite.svg "$(DESTDIR)$(ICONDIR)/kite.svg"
	install -d "$(DESTDIR)$(DATADIR)/icons"
	install -m 644 data/kite.svg "$(DESTDIR)$(DATADIR)/icons/kite.svg"
	-python3 -c "import os, gi; gi.require_version('GdkPixbuf', '2.0'); from gi.repository import GdkPixbuf; \
		svg='data/kite.svg'; \
		[os.makedirs(f'$(DESTDIR)$(DATADIR)/icons/hicolor/{s}x{s}/apps', exist_ok=True) or GdkPixbuf.Pixbuf.new_from_file_at_scale(svg, s, s, True).savev(f'$(DESTDIR)$(DATADIR)/icons/hicolor/{s}x{s}/apps/kite.png', 'png', [], []) for s in [16, 24, 32, 48, 64, 128, 256]]; \
		GdkPixbuf.Pixbuf.new_from_file_at_scale(svg, 256, 256, True).savev('$(DESTDIR)$(DATADIR)/icons/kite.png', 'png', [], [])" 2>/dev/null || true
	-update-desktop-database "$(DESTDIR)$(APPDIR)" 2>/dev/null || true
	-gtk-update-icon-cache -f -t "$(DESTDIR)$(DATADIR)/icons/hicolor" 2>/dev/null || true
	@if [ -z "$(DESTDIR)" ] && [ -d "$(HOME)/Desktop" ]; then \
		sed 's|@EXEC@|$(BINDIR)/kite|g' data/kite.desktop.in > "$(HOME)/Desktop/kite.desktop"; \
		chmod 755 "$(HOME)/Desktop/kite.desktop"; \
	fi
	@echo "Installed Kite to $(BINDIR)/kite."
	@echo "To make Kite the default browser, run: make default-browser"

default-browser:
	@command -v xdg-settings >/dev/null 2>&1 || { echo "xdg-settings is not installed."; exit 1; }
	xdg-settings set default-web-browser kite.desktop
	@echo "Kite is now registered as the default web browser for this desktop."

clean:
	rm -rf $(BUILD_DIR) mybrowser mybrowser-debug
