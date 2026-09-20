CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -pthread -flto -march=native
LDFLAGS ?=
LDLIBS ?= -lsystemd -lz -lwayland-client -pthread
UDEV_DIR ?= /etc/udev/rules.d
SYSTEMD_SYSTEM_DIR ?= /etc/systemd/system
CONFIG_DIR ?= $(HOME)/.config/wayland-zeroprint
INSTALL_USER ?= $(shell id -un)
INSTALL_UID ?= $(shell id -u)

TARGET = build/wayland-zeroprint
INSTALL_TARGET = /usr/local/bin/wayland-zeroprint
SERVICE_TEMPLATE = $(SYSTEMD_SYSTEM_DIR)/wayland-zeroprint@.service
PATH_TEMPLATE = $(SYSTEMD_SYSTEM_DIR)/wayland-zeroprint@.path
SERVICE_INSTANCE = wayland-zeroprint@$(INSTALL_UID).service
PATH_INSTANCE = wayland-zeroprint@$(INSTALL_UID).path
WAYLAND_SCANNER ?= wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols)
EXT_DATA_CONTROL_XML = $(WAYLAND_PROTOCOLS_DIR)/staging/ext-data-control/ext-data-control-v1.xml
EXT_DATA_CONTROL_HEADER = build/ext-data-control-v1-client-protocol.h
EXT_DATA_CONTROL_CODE = build/ext-data-control-v1-protocol.c
SRCS = src/main.c src/clipboard-wayland.c $(EXT_DATA_CONTROL_CODE)
CPPFLAGS += -Ibuild

.PHONY: all clean test install install-bin install-config install-service install-security install-udev install-kwin-env uninstall benchmark
.NOTPARALLEL: install

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
	@tests/test-systemd.sh
	@tests/test-install-security.sh

install-bin: $(TARGET)
	sudo install -d /usr/local/bin
	sudo install -m 755 $(TARGET) $(INSTALL_TARGET)

install-config:
	install -d $(CONFIG_DIR)
	@test -e $(CONFIG_DIR)/config || install -m 644 config/wayland-zeroprint.conf $(CONFIG_DIR)/config

install-kwin-env:
	@mkdir -p $(HOME)/.config/environment.d
	@echo "KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1" > $(HOME)/.config/environment.d/10-kwin-screenshot.conf
	@echo "Configured KWin direct screenshot permission environment."

install-security:
	@if test "$(INSTALL_UID)" = 0; then \
		echo "Run make install as the desktop user, not as root."; exit 1; \
	fi
	@if id -nG "$(INSTALL_USER)" | tr ' ' '\n' | grep -qx input; then \
		echo "$(INSTALL_USER) belongs to input; review other tools and remove membership yourself before installing."; exit 1; \
	fi
	@if id -nG | tr ' ' '\n' | grep -qx input; then \
		echo "This login session still has input from an earlier group list; log out and back in before installing."; exit 1; \
	fi

install-service:
	-systemctl --user disable --now wayland-zeroprint.service
	rm -f $(HOME)/.config/systemd/user/wayland-zeroprint.service
	rm -f $(HOME)/.local/bin/wayland-zeroprint
	systemctl --user daemon-reload
	sudo install -d $(SYSTEMD_SYSTEM_DIR)
	sudo install -m 644 systemd/wayland-zeroprint@.service $(SERVICE_TEMPLATE)
	sudo install -m 644 systemd/wayland-zeroprint@.path $(PATH_TEMPLATE)
	sudo systemctl daemon-reload
	sudo systemctl enable --now $(PATH_INSTANCE)
	@if test -S /run/user/$(INSTALL_UID)/wayland-0; then \
		sudo systemctl restart $(SERVICE_INSTANCE); \
	fi

install-udev:
	sudo install -d $(UDEV_DIR)
	sudo install -m 644 udev/99-wayland-zeroprint.rules $(UDEV_DIR)/99-wayland-zeroprint.rules
	sudo udevadm control --reload-rules
	sudo udevadm trigger --subsystem-match=input

install-bin install-config install-kwin-env install-udev install-service: install-security

install: install-bin install-config install-kwin-env install-udev install-service

uninstall:
	-sudo systemctl disable --now $(SERVICE_INSTANCE)
	-sudo systemctl disable --now $(PATH_INSTANCE)
	-sudo rm -f $(SERVICE_TEMPLATE)
	-sudo rm -f $(PATH_TEMPLATE)
	-sudo rm -f $(INSTALL_TARGET)
	-systemctl --user disable --now wayland-zeroprint.service
	rm -f $(HOME)/.config/systemd/user/wayland-zeroprint.service
	rm -f $(HOME)/.config/environment.d/10-kwin-screenshot.conf
	sudo systemctl daemon-reload
	systemctl --user daemon-reload
	-sudo rm -f $(UDEV_DIR)/99-wayland-zeroprint.rules
	-sudo udevadm control --reload-rules

clean:
	rm -f $(TARGET) $(EXT_DATA_CONTROL_HEADER) $(EXT_DATA_CONTROL_CODE)
