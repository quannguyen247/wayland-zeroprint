PREFIX ?= $(HOME)/.local
UDEV_DIR ?= /etc/udev/rules.d
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user

.PHONY: all install install-user install-udev uninstall clean

all:
	@echo "wayland-zeroprint does not require compilation."
	@echo "Run 'make install-user' or './install.sh' to install."

install-user:
	install -d $(PREFIX)/bin
	install -m 755 bin/wayland-zeroprint $(PREFIX)/bin/wayland-zeroprint
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
	systemctl --user daemon-reload
	-sudo rm -f $(UDEV_DIR)/99-wayland-zeroprint.rules
	-sudo udevadm control --reload-rules
