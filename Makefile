FQBN := esp32:esp32:esp32s3:USBMode=default,FlashSize=16M,PSRAM=opi
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*))
ESP32_CORE ?= $(shell arduino-cli config get directories.data)/packages/esp32/hardware/esp32/3.3.12
# Arduino's bundled ctags is x86-only on Apple Silicon; CI sets /usr/bin.
CTAGS_DIR ?= /opt/homebrew/bin

# Board x app set (docs/architecture.md, "App sets and builds").
# `make build BOARD=waveshare_169 APPS=hub-loader`; `make site` builds them all.
BOARDS := waveshare_169
APP_SETS := hub hub-loader everything
BOARD ?= waveshare_169
APPS ?= hub
APP_FLAGS_hub :=
APP_FLAGS_hub-loader := -DAPP_LOADER=1
APP_FLAGS_everything := -DAPP_LOADER=1 -DAPP_RECORDER=1 -DAPP_WEB_UI=1
# WRITES=1 lets the Loader write the FM-1's flash (FM1_LOADER_WRITES, rom.h).
# Off by default: on only for hardware checkpoints 2 and 3 until they pass.
WRITES ?= 0
WRITE_FLAGS := $(if $(filter 1,$(WRITES)),-DFM1_LOADER_WRITES=1)
APP_LIBS_hub :=
APP_LIBS_hub-loader := --library lib/fm1_loader
APP_LIBS_everything := --library lib/fm1_loader
SKETCH := firmware/$(BOARD)
BUILD := build/$(BOARD)/$(APPS)
# Mac tests use the Mac's clang; CI sets clang++ too (GCC's -Wformat-truncation
# objects to the deliberate cut-off of screen lines).
TEST_CXX ?= c++
SITE_PORT ?= 8088
# The app0 slot of firmware/<board>/partitions.csv (3 MB); Arduino's own limit is
# the default table's 1.25 MB.
APP_SLOT := 3145728
STORE_OFFSET := 0x3D0000
# Shown on the computer-mode screen, and makes every commit a distinct build.
FW_VERSION := 0x$(shell git rev-parse --short=7 HEAD)
FW_DIRTY := $(if $(shell git status --porcelain --untracked-files=no),1,0)

.PHONY: test build build-all flash site serve clean fixtures golden store-image flash-store store-add

FM1_SRC := $(wildcard lib/fm1_loader/src/*.cpp)
FM1_HDR := $(wildcard lib/fm1_loader/src/*.h) $(wildcard tests/support/*.h)
FM1_TESTS := build/test_fm1_package build/test_fm1_library build/test_fm1_installer \
	build/test_fm1_softkey build/test_fm1_posix build/test_fm1_locked build/test_fm1_library_posix

test: build/test_fm1_interop build/test_midi_core build/test_status build/test_boot_mode build/test_broker build/test_describe $(FM1_TESTS) fixtures
	./build/test_midi_core
	./build/test_status
	./build/test_boot_mode
	./build/test_broker
	./build/test_describe
	for t in $(FM1_TESTS); do ./$$t || exit 1; done
	python3 -c "import shutil; shutil.rmtree('build/interop', ignore_errors=True)"
	python3 -I tools/fm1lib.py selftest build/interop tests/fixtures/skiff-1.4.fwsc \
		tests/fixtures/sloop-2.8.1.fwsc tests/fixtures/FM-1_015.fwsc
	./build/test_fm1_interop build/interop
	python3 -I tools/check_partitions.py firmware/$(BOARD)/partitions.csv

# The Mac tests may write the mock FM-1 (FM1_LOADER_WRITES=1); test_fm1_locked
# is built without it, as every firmware build is until hardware checkpoint 3.
FM1_TEST_SRC := $(FM1_SRC) $(wildcard tests/support/*.cpp)
build/test_fm1_locked: tests/test_fm1_locked.cpp $(FM1_TEST_SRC) $(FM1_HDR)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/fm1_loader/src -Itests -o $@ $< $(FM1_TEST_SRC)

build/test_fm1_library_posix: tests/test_fm1_library.cpp $(FM1_TEST_SRC) $(FM1_HDR)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -DFM1_LOADER_WRITES=1 -DTEST_POSIX_STORAGE -Ilib/fm1_loader/src -Itests -o $@ $< $(FM1_TEST_SRC)

build/test_fm1_%: tests/test_fm1_%.cpp $(FM1_TEST_SRC) $(FM1_HDR)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -DFM1_LOADER_WRITES=1 -Ilib/fm1_loader/src -Itests -o $@ $< $(FM1_TEST_SRC)

# Packages and the loader blob for the fm1_loader tests, pinned by SHA-256 and
# kept out of git (tests/fixtures/).
fixtures:
	python3 -I tools/fetch_fixtures.py tests/fixtures

# Regenerates tests/golden/ from the Unbricker and jl-uboot-tool (Python), then
# shows any difference from the committed vectors. Only needed when the
# reference changes; make test reads the committed files.
GOLDEN_PY := build/golden-venv/bin/python
$(GOLDEN_PY):
	python3 -m venv build/golden-venv
	$(GOLDEN_PY) -m pip install -q crcmod==1.7 pyyaml
golden: $(GOLDEN_PY) fixtures
	$(GOLDEN_PY) tools/gen_golden.py tests/golden
	git diff --stat --exit-code -- tests/golden

build/test_midi_core: tests/test_midi_core.cpp lib/midi_core/src/midi_core.cpp lib/midi_core/src/midi_core.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_midi_core.cpp lib/midi_core/src/midi_core.cpp

build/test_status: tests/test_status.cpp lib/midi_core/src/status.cpp lib/midi_core/src/status.h lib/midi_core/src/midi_core.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_status.cpp lib/midi_core/src/status.cpp lib/midi_core/src/midi_core.cpp

build/test_boot_mode: tests/test_boot_mode.cpp lib/midi_core/src/boot_mode.cpp lib/midi_core/src/boot_mode.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_boot_mode.cpp lib/midi_core/src/boot_mode.cpp

build/test_broker: tests/test_broker.cpp lib/platform/src/broker.cpp lib/platform/src/broker.h lib/midi_core/src/midi_core.cpp lib/midi_core/src/midi_core.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/platform/src -Ilib/midi_core/src -o $@ tests/test_broker.cpp lib/platform/src/broker.cpp lib/midi_core/src/midi_core.cpp

build/test_describe: tests/test_describe.cpp lib/platform/src/describe.cpp lib/platform/src/describe.h lib/platform/src/broker.cpp lib/platform/src/broker.h lib/midi_core/src/midi_core.cpp
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/platform/src -Ilib/midi_core/src -o $@ tests/test_describe.cpp lib/platform/src/describe.cpp lib/platform/src/broker.cpp lib/midi_core/src/midi_core.cpp

build:
	$(if $(filter $(APPS),$(APP_SETS)),,$(error APPS=$(APPS); known: $(APP_SETS)))
	arduino-cli compile --fqbn $(FQBN) --build-property "tools.ctags.path=$(CTAGS_DIR)" \
		--build-property "upload.maximum_size=$(APP_SLOT)" \
		--build-property "compiler.cpp.extra_flags=-DFW_VERSION=$(FW_VERSION) -DFW_DIRTY=$(FW_DIRTY) -DESP_USB_HOST_MAX_SERIAL_PORTS=0 $(APP_FLAGS_$(APPS)) $(WRITE_FLAGS) -DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--library lib/midi_core --library lib/EspUsbHost --library lib/host_ui --library lib/platform $(APP_LIBS_$(APPS)) \
		--output-dir $(BUILD) $(SKETCH)

build-all:
	for b in $(BOARDS); do for a in $(APP_SETS); do $(MAKE) build BOARD=$$b APPS=$$a || exit 1; done; done

flash: build
	arduino-cli upload --fqbn $(FQBN) --input-dir $(BUILD) --port $(PORT) $(SKETCH)

# The store partition's first setup: a LittleFS image holding JieLi's loader
# (from tests/fixtures). JieLi's code, so it stays in build/ and is never staged
# into site/. Flashing it ERASES every backup and save in the store, so it asks
# for CONFIRM=erase-store. The board must be in download mode (Computer mode).
TOOLS_PY := build/tools-venv/bin/python
$(TOOLS_PY):
	python3 -m venv build/tools-venv
	$(TOOLS_PY) -m pip install -q littlefs-python==0.19.0
store-image: $(TOOLS_PY) fixtures
	$(TOOLS_PY) tools/store_image.py firmware/$(BOARD)/partitions.csv build/store/store.bin
flash-store: store-image
	$(if $(filter erase-store,$(CONFIRM)),,$(error flash-store erases every backup in the store: add CONFIRM=erase-store))
	$(firstword $(wildcard $(ESP32_CORE)/../../../tools/esptool_py/*/esptool)) --chip esp32s3 --port $(PORT) \
		write-flash $(STORE_OFFSET) build/store/store.bin

# Adds packages to the board's store without losing its backups and saves:
# reads the store back (kept in build/store/), adds, checks nothing else changed,
# writes it back. Board in Computer mode. make store-add PKGS="a.fwsc b.fwsc" [PIN=1]
store-add: $(TOOLS_PY)
	$(if $(PKGS),,$(error PKGS="file.fwsc ..." names the packages to add))
	$(TOOLS_PY) tools/store_add.py firmware/$(BOARD)/partitions.csv $(PORT) $(if $(PIN),--pin) $(PKGS)

# Stage the firmware into site/ for the browser installer (ESP Web Tools).
# fixtures brings wl82loader.bin, so build_site can scan for pieces of it.
site: build-all fixtures
	for b in $(BOARDS); do for a in $(APP_SETS); do \
		python3 tools/build_site.py $$b $$a build/$$b/$$a $(ESP32_CORE)/tools/partitions/boot_app0.bin \
			$(shell git describe --always --dirty) || exit 1; done; done

# Web Serial needs a secure context; http://localhost counts.
serve: site
	python3 -m http.server $(SITE_PORT) --bind 127.0.0.1 --directory site

clean:
	rm -rf build
