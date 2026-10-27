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
APP_LIBS_hub :=
APP_LIBS_hub-loader := --library lib/fm1_loader
APP_LIBS_everything := --library lib/fm1_loader
SKETCH := firmware/$(BOARD)
BUILD := build/$(BOARD)/$(APPS)
# Mac tests use the Mac's clang; CI sets clang++ too (GCC's -Wformat-truncation
# objects to the deliberate cut-off of screen lines).
TEST_CXX ?= c++
SITE_PORT ?= 8088
# Shown on the computer-mode screen, and makes every commit a distinct build.
FW_VERSION := 0x$(shell git rev-parse --short=7 HEAD)
FW_DIRTY := $(if $(shell git status --porcelain --untracked-files=no),1,0)

.PHONY: test build build-all flash site serve clean fixtures golden

FM1_SRC := $(wildcard lib/fm1_loader/src/*.cpp)
FM1_HDR := $(wildcard lib/fm1_loader/src/*.h) $(wildcard tests/support/*.h)
FM1_TESTS := build/test_fm1_package build/test_fm1_library build/test_fm1_installer \
	build/test_fm1_softkey build/test_fm1_posix build/test_fm1_locked build/test_fm1_library_posix

test: build/test_midi_core build/test_status build/test_boot_mode build/test_broker build/test_describe $(FM1_TESTS) fixtures
	./build/test_midi_core
	./build/test_status
	./build/test_boot_mode
	./build/test_broker
	./build/test_describe
	for t in $(FM1_TESTS); do ./$$t || exit 1; done

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
		--build-property "compiler.cpp.extra_flags=-DFW_VERSION=$(FW_VERSION) -DFW_DIRTY=$(FW_DIRTY) -DESP_USB_HOST_MAX_SERIAL_PORTS=0 $(APP_FLAGS_$(APPS)) -DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--library lib/midi_core --library lib/EspUsbHost --library lib/host_ui --library lib/platform $(APP_LIBS_$(APPS)) \
		--output-dir $(BUILD) $(SKETCH)

build-all:
	for b in $(BOARDS); do for a in $(APP_SETS); do $(MAKE) build BOARD=$$b APPS=$$a || exit 1; done; done

flash: build
	arduino-cli upload --fqbn $(FQBN) --input-dir $(BUILD) --port $(PORT) $(SKETCH)

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
