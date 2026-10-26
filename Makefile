FQBN := esp32:esp32:esp32s3:USBMode=default,FlashSize=16M,PSRAM=opi
SKETCH := firmware/waveshare_169
BUILD := build/waveshare_169
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*))
ESP32_CORE := $(HOME)/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12
SITE_PORT ?= 8088
# Shown on the computer-mode screen, and makes every commit a distinct build.
FW_VERSION := 0x$(shell git rev-parse --short=7 HEAD)
FW_DIRTY := $(if $(shell git status --porcelain --untracked-files=no),1,0)

.PHONY: test build flash site serve clean fixtures golden

FM1_SRC := $(wildcard lib/fm1_loader/src/*.cpp)
FM1_HDR := $(wildcard lib/fm1_loader/src/*.h) $(wildcard tests/support/*.h)
FM1_TESTS := build/test_fm1_package build/test_fm1_library build/test_fm1_installer

test: build/test_midi_core build/test_status build/test_boot_mode $(FM1_TESTS) fixtures
	./build/test_midi_core
	./build/test_status
	./build/test_boot_mode
	for t in $(FM1_TESTS); do ./$$t || exit 1; done

build/test_fm1_%: tests/test_fm1_%.cpp $(FM1_SRC) $(FM1_HDR) $(wildcard tests/support/*.cpp)
	mkdir -p build
	c++ -std=c++17 -Wall -Wextra -Werror -Ilib/fm1_loader/src -Itests -o $@ $< $(FM1_SRC) $(wildcard tests/support/*.cpp)

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
	c++ -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_midi_core.cpp lib/midi_core/src/midi_core.cpp

build/test_status: tests/test_status.cpp lib/midi_core/src/status.cpp lib/midi_core/src/status.h lib/midi_core/src/midi_core.h
	mkdir -p build
	c++ -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_status.cpp lib/midi_core/src/status.cpp lib/midi_core/src/midi_core.cpp

build/test_boot_mode: tests/test_boot_mode.cpp lib/midi_core/src/boot_mode.cpp lib/midi_core/src/boot_mode.h
	mkdir -p build
	c++ -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_boot_mode.cpp lib/midi_core/src/boot_mode.cpp

build:
	arduino-cli compile --fqbn $(FQBN) --build-property "tools.ctags.path=/opt/homebrew/bin" \
		--build-property "compiler.cpp.extra_flags=-DFW_VERSION=$(FW_VERSION) -DFW_DIRTY=$(FW_DIRTY) -DESP_USB_HOST_MAX_SERIAL_PORTS=0 -DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--library lib/midi_core --library lib/EspUsbHost --library lib/host_ui --output-dir $(BUILD) $(SKETCH)

flash: build
	arduino-cli upload --fqbn $(FQBN) --input-dir $(BUILD) --port $(PORT) $(SKETCH)

# Stage the firmware into site/ for the browser installer (ESP Web Tools).
# fixtures brings wl82loader.bin, so build_site can scan for pieces of it.
site: build fixtures
	python3 tools/build_site.py waveshare-esp32-s3-touch-lcd-1.69 $(BUILD) $(ESP32_CORE)/tools/partitions/boot_app0.bin \
		$(shell git describe --always --dirty)

# Web Serial needs a secure context; http://localhost counts.
serve: site
	python3 -m http.server $(SITE_PORT) --bind 127.0.0.1 --directory site

clean:
	rm -rf build
