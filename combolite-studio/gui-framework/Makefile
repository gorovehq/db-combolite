PREFIX ?= /usr/local
DATA_DIR ?= $(PREFIX)/share
PYTHON ?= python3

.PHONY: all wheel install-user install-system uninstall-user uninstall-system test clean

all: wheel

wheel:
	$(PYTHON) -m pip wheel --no-build-isolation --no-deps -w dist .

test:
	$(PYTHON) -m unittest discover -v -s tests

install-user:
	@mkdir -p ~/.local/share/applications \
	          ~/.local/share/metainfo \
	          ~/.local/share/mime/packages \
	          ~/.local/share/icons/hicolor/scalable/apps \
	          ~/.local/share/icons/hicolor/symbolic/apps
	cp data/io.github.gorovehq.ComboliteStudio.desktop ~/.local/share/applications/
	cp data/io.github.gorovehq.ComboliteStudio.metainfo.xml ~/.local/share/metainfo/
	cp data/io.github.gorovehq.ComboliteStudio.mime.xml ~/.local/share/mime/packages/
	cp data/icons/hicolor/scalable/apps/io.github.gorovehq.ComboliteStudio.svg ~/.local/share/icons/hicolor/scalable/apps/
	cp data/icons/hicolor/symbolic/apps/io.github.gorovehq.ComboliteStudio-symbolic.svg ~/.local/share/icons/hicolor/symbolic/apps/
	@command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database ~/.local/share/applications || true
	@command -v update-mime-database >/dev/null 2>&1 && update-mime-database ~/.local/share/mime || true
	@command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t ~/.local/share/icons/hicolor || true
	@echo "Desktop and icon assets installed to ~/.local/share"

uninstall-user:
	rm -f ~/.local/share/applications/io.github.gorovehq.ComboliteStudio.desktop
	rm -f ~/.local/share/metainfo/io.github.gorovehq.ComboliteStudio.metainfo.xml
	rm -f ~/.local/share/mime/packages/io.github.gorovehq.ComboliteStudio.mime.xml
	rm -f ~/.local/share/icons/hicolor/scalable/apps/io.github.gorovehq.ComboliteStudio.svg
	rm -f ~/.local/share/icons/hicolor/symbolic/apps/io.github.gorovehq.ComboliteStudio-symbolic.svg
	@command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database ~/.local/share/applications || true
	@command -v update-mime-database >/dev/null 2>&1 && update-mime-database ~/.local/share/mime || true
	@command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t ~/.local/share/icons/hicolor || true

install-system:
	install -d $(DATA_DIR)/applications \
	           $(DATA_DIR)/metainfo \
	           $(DATA_DIR)/mime/packages \
	           $(DATA_DIR)/icons/hicolor/scalable/apps \
	           $(DATA_DIR)/icons/hicolor/symbolic/apps
	install -m 644 data/io.github.gorovehq.ComboliteStudio.desktop $(DATA_DIR)/applications/
	install -m 644 data/io.github.gorovehq.ComboliteStudio.metainfo.xml $(DATA_DIR)/metainfo/
	install -m 644 data/io.github.gorovehq.ComboliteStudio.mime.xml $(DATA_DIR)/mime/packages/
	install -m 644 data/icons/hicolor/scalable/apps/io.github.gorovehq.ComboliteStudio.svg $(DATA_DIR)/icons/hicolor/scalable/apps/
	install -m 644 data/icons/hicolor/symbolic/apps/io.github.gorovehq.ComboliteStudio-symbolic.svg $(DATA_DIR)/icons/hicolor/symbolic/apps/
	@command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database $(DATA_DIR)/applications || true
	@command -v update-mime-database >/dev/null 2>&1 && update-mime-database $(DATA_DIR)/mime || true
	@command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t $(DATA_DIR)/icons/hicolor || true

clean:
	rm -rf dist build *.egg-info combolite_gui/libcombolite.so
