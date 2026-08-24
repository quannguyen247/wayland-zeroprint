CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -pthread -flto -march=native
LDFLAGS ?=
LDLIBS ?= -lsystemd -lz -pthread
PREFIX ?= $(HOME)/.local
UDEV_DIR ?= /etc/udev/rules.d
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user

TARGET = $(PREFIX)/bin/wayland-zeroprint
SRCS = src/main.c

.PHONY: all clean install install-user install-udev install-kwin-env uninstall benchmark

all: $(TARGET)

$(TARGET): $(SRCS)
	@mkdir -p $(PREFIX)/bin
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c $(LDFLAGS) $(LDLIBS) -o $(TARGET)
	@chmod 755 $(TARGET)
	@echo "Built and installed $(TARGET) successfully."

benchmark: $(TARGET)
	@$(TARGET) --benchmark

install-kwin-env:
	@mkdir -p $(HOME)/.config/environment.d
	@echo "KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1" > $(HOME)/.config/environment.d/10-kwin-screenshot.conf
	@echo "Configured KWin direct screenshot permission environment."

install-user: $(TARGET) install-kwin-env
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
	rm -f $(PREFIX)/bin/wayland-zeroprint
	rm -f $(SYSTEMD_USER_DIR)/wayland-zeroprint.service
	rm -f $(HOME)/.config/environment.d/10-kwin-screenshot.conf
	systemctl --user daemon-reload
	-sudo rm -f $(UDEV_DIR)/99-wayland-zeroprint.rules
	-sudo udevadm control --reload-rules

clean:
	rm -f $(TARGET)
