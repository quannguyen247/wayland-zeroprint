CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -pthread -flto -march=native
LDFLAGS ?=
LDLIBS ?= -lsystemd -lz -lwayland-client -pthread
PREFIX ?= $(HOME)/.local
UDEV_DIR ?= /etc/udev/rules.d
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user
CONFIG_DIR ?= $(HOME)/.config/wayland-zeroprint

TARGET = build/wayland-zeroprint
INSTALL_TARGET = $(PREFIX)/bin/wayland-zeroprint
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
EXT_DATA_CONTROL_XML = $(WAYLAND_PROTOCOLS_DIR)/staging/ext-data-control/ext-data-control-v1.xml
EXT_DATA_CONTROL_HEADER = build/ext-data-control-v1-client-protocol.h
EXT_DATA_CONTROL_CODE = build/ext-data-control-v1-protocol.c
SRCS = src/main.c src/clipboard-wayland.c $(EXT_DATA_CONTROL_CODE)
CPPFLAGS += -Ibuild

.PHONY: all clean test install install-bin install-config install-user install-udev install-kwin-env uninstall benchmark

all: $(TARGET)

$(EXT_DATA_CONTROL_HEADER): $(EXT_DATA_CONTROL_XML)
	@mkdir -p build
	$(WAYLAND_SCANNER) client-header $< $@

$(EXT_DATA_CONTROL_CODE): $(EXT_DATA_CONTROL_XML)
	@mkdir -p build
	$(WAYLAND_SCANNER) private-code $< $@

$(TARGET): $(SRCS) $(EXT_DATA_CONTROL_HEADER)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRCS) $(LDFLAGS) $(LDLIBS) -o $(TARGET)
	@chmod 755 $(TARGET)
	@echo "Built $(TARGET) successfully."

benchmark: $(TARGET)
	@$(TARGET) --benchmark

test: $(TARGET)
	@$(TARGET) --self-test
	@tests/test-cli.sh $(TARGET)

install-bin: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(INSTALL_TARGET)

install-config:
	install -d $(CONFIG_DIR)
	@test -e $(CONFIG_DIR)/config || install -m 644 config/wayland-zeroprint.conf $(CONFIG_DIR)/config

install-kwin-env:
	@mkdir -p $(HOME)/.config/environment.d
	@echo "KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1" > $(HOME)/.config/environment.d/10-kwin-screenshot.conf
	@echo "Configured KWin direct screenshot permission environment."

install-user: install-bin install-config install-kwin-env
	install -d $(SYSTEMD_USER_DIR)
	install -m 644 systemd/wayland-zeroprint.service $(SYSTEMD_USER_DIR)/wayland-zeroprint.service
	systemctl --user daemon-reload
	systemctl --user enable --now wayland-zeroprint.service

install-udev:
	sudo install -d $(UDEV_DIR)
	sudo install -m 644 udev/99-wayland-zeroprint.rules $(UDEV_DIR)/99-wayland-zeroprint.rules
	sudo udevadm control --reload-rules
	sudo udevadm trigger --subsystem-match=input

install: install-udev install-user

uninstall:
	-systemctl --user stop wayland-zeroprint.service
	-systemctl --user disable wayland-zeroprint.service
	rm -f $(INSTALL_TARGET)
	rm -f $(SYSTEMD_USER_DIR)/wayland-zeroprint.service
	rm -f $(HOME)/.config/environment.d/10-kwin-screenshot.conf
	systemctl --user daemon-reload
	-sudo rm -f $(UDEV_DIR)/99-wayland-zeroprint.rules
	-sudo udevadm control --reload-rules

clean:
	rm -f $(TARGET) $(EXT_DATA_CONTROL_HEADER) $(EXT_DATA_CONTROL_CODE)
