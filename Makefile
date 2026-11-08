CC ?= gcc
CXX ?= g++
CFLAGS ?= -O3 -march=native -Wall -Wextra -std=c11
CXXFLAGS ?= -O3 -march=native -Wall -Wextra -std=c++20
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
SYSTEMD_USER_DIR ?= $(PREFIX)/lib/systemd/user

PW_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3)
PW_LIBS   := $(shell pkg-config --libs libpipewire-0.3)
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0)
MATH_LIBS  := -lm -lzita-convolver -lstdc++

BUILDDIR := build
BIN_DAEMON := $(BUILDDIR)/litefxd
BIN_CTL    := $(BUILDDIR)/litefxctl
BIN_GUI    := $(BUILDDIR)/litefx
BIN_TEST   := $(BUILDDIR)/test_dsp

COMMON_SRCS := src/dsp.c src/config.c
ZITA_OBJ    := $(BUILDDIR)/crystalizer_zita.o

.PHONY: all clean test install install-user uninstall

all: $(BIN_DAEMON) $(BIN_CTL) $(BIN_GUI)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(ZITA_OBJ): src/crystalizer_zita.cpp src/crystalizer_zita.h src/common.h | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c src/crystalizer_zita.cpp -o $@

$(BIN_DAEMON): src/daemon.c $(COMMON_SRCS) $(ZITA_OBJ) src/common.h src/dsp.h src/config.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(PW_CFLAGS) src/daemon.c $(COMMON_SRCS) $(ZITA_OBJ) $(PW_LIBS) $(MATH_LIBS) -o $@

$(BIN_CTL): src/ctl.c src/config.c src/common.h src/config.h | $(BUILDDIR)
	$(CC) $(CFLAGS) src/ctl.c src/config.c -lm -o $@

$(BIN_GUI): src/gui.c $(COMMON_SRCS) $(ZITA_OBJ) src/common.h src/dsp.h src/config.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) src/gui.c $(COMMON_SRCS) $(ZITA_OBJ) $(GTK_LIBS) $(MATH_LIBS) -o $@

$(BIN_TEST): tests/test_dsp.c $(COMMON_SRCS) $(ZITA_OBJ) src/common.h src/dsp.h src/config.h | $(BUILDDIR)
	$(CC) $(CFLAGS) tests/test_dsp.c $(COMMON_SRCS) $(ZITA_OBJ) $(MATH_LIBS) -o $@

test: $(BIN_TEST)
	@echo "Running LiteFX DSP verification suite..."
	@./$(BIN_TEST)

clean:
	rm -rf $(BUILDDIR)

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN_DAEMON) $(DESTDIR)$(BINDIR)/litefxd
	install -m 755 $(BIN_CTL) $(DESTDIR)$(BINDIR)/litefxctl
	install -m 755 $(BIN_GUI) $(DESTDIR)$(BINDIR)/litefx
	install -d $(DESTDIR)$(DATADIR)/applications
	sed 's|Exec=litefx|Exec=$(BINDIR)/litefx|g; s|TryExec=litefx|TryExec=$(BINDIR)/litefx|g' data/litefx.desktop > $(DESTDIR)$(DATADIR)/applications/litefx.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/applications/litefx.desktop
	install -d $(DESTDIR)$(DATADIR)/icons/hicolor/512x512/apps
	install -m 644 data/litefx.png $(DESTDIR)$(DATADIR)/icons/hicolor/512x512/apps/litefx.png
	install -d $(DESTDIR)$(DATADIR)/litefx/presets
	install -m 644 data/presets/*.conf $(DESTDIR)$(DATADIR)/litefx/presets/
	install -m 644 data/autoeq_index.txt $(DESTDIR)$(DATADIR)/litefx/autoeq_index.txt
	install -d $(DESTDIR)$(SYSTEMD_USER_DIR)
	sed 's|/usr/local/bin|$(BINDIR)|g' data/litefxd.service > $(DESTDIR)$(SYSTEMD_USER_DIR)/litefxd.service
	chmod 644 $(DESTDIR)$(SYSTEMD_USER_DIR)/litefxd.service

install-user: all
	install -d $(HOME)/.local/bin
	install -m 755 $(BIN_DAEMON) $(HOME)/.local/bin/litefxd
	install -m 755 $(BIN_CTL) $(HOME)/.local/bin/litefxctl
	install -m 755 $(BIN_GUI) $(HOME)/.local/bin/litefx
	install -d $(HOME)/.local/share/applications
	sed 's|Exec=litefx|Exec=$(HOME)/.local/bin/litefx|g; s|TryExec=litefx|TryExec=$(HOME)/.local/bin/litefx|g' data/litefx.desktop > $(HOME)/.local/share/applications/litefx.desktop
	chmod 644 $(HOME)/.local/share/applications/litefx.desktop
	install -d $(HOME)/Desktop
	sed 's|Exec=litefx|Exec=$(HOME)/.local/bin/litefx|g; s|TryExec=litefx|TryExec=$(HOME)/.local/bin/litefx|g' data/litefx.desktop > $(HOME)/Desktop/litefx.desktop
	chmod 755 $(HOME)/Desktop/litefx.desktop
	install -d $(HOME)/.local/share/icons/hicolor/512x512/apps
	install -m 644 data/litefx.png $(HOME)/.local/share/icons/hicolor/512x512/apps/litefx.png
	install -m 644 data/litefx.png $(HOME)/.local/share/icons/litefx.png
	install -d $(HOME)/.config/litefx/presets
	install -m 644 data/presets/*.conf $(HOME)/.config/litefx/presets/
	install -d $(HOME)/.local/share/litefx
	install -m 644 data/autoeq_index.txt $(HOME)/.local/share/litefx/autoeq_index.txt
	install -d $(HOME)/.cache/litefx
	install -m 644 data/autoeq_index.txt $(HOME)/.cache/litefx/autoeq_index.txt
	install -d $(HOME)/.config/systemd/user
	sed 's|/usr/local/bin|$(HOME)/.local/bin|g' data/litefxd.service > $(HOME)/.config/systemd/user/litefxd.service
	systemctl --user daemon-reload 2>/dev/null || true
	update-desktop-database $(HOME)/.local/share/applications 2>/dev/null || true
	gtk-update-icon-cache $(HOME)/.local/share/icons/hicolor 2>/dev/null || true
	@echo "LiteFX installed to ~/.local/bin and desktop shortcut created!"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/litefxd
	rm -f $(DESTDIR)$(BINDIR)/litefxctl
	rm -f $(DESTDIR)$(BINDIR)/litefx
	rm -f $(DESTDIR)$(DATADIR)/applications/litefx.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/512x512/apps/litefx.png
	rm -rf $(DESTDIR)$(DATADIR)/litefx
	rm -f $(DESTDIR)$(SYSTEMD_USER_DIR)/litefxd.service
