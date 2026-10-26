FQBN := esp32:esp32:esp32s3:USBMode=default,FlashSize=16M,PSRAM=opi
SKETCH := firmware/waveshare_169
BUILD := build/waveshare_169
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*))

.PHONY: test build flash clean fixtures golden

FM1_SRC := $(wildcard lib/fm1_loader/src/*.cpp)
FM1_HDR := $(wildcard lib/fm1_loader/src/*.h) $(wildcard tests/support/*.h)
FM1_TESTS := build/test_fm1_package build/test_fm1_library build/test_fm1_installer

test: build/test_midi_core build/test_status $(FM1_TESTS) fixtures
	./build/test_midi_core
	./build/test_status
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

build:
	arduino-cli compile --fqbn $(FQBN) --build-property "tools.ctags.path=/opt/homebrew/bin" \
		--build-property "compiler.cpp.extra_flags=-DESP_USB_HOST_MAX_SERIAL_PORTS=0 -DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--library lib/midi_core --library lib/EspUsbHost --library lib/host_ui --output-dir $(BUILD) $(SKETCH)

flash: build
	arduino-cli upload --fqbn $(FQBN) --input-dir $(BUILD) --port $(PORT) $(SKETCH)

clean:
	rm -rf build
