FQBN := esp32:esp32:esp32s3:USBMode=default,FlashSize=16M,PSRAM=opi
SKETCH := firmware/waveshare_169
BUILD := build/waveshare_169
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*))

.PHONY: test build flash clean

test: build/test_midi_core
	./build/test_midi_core

build/test_midi_core: tests/test_midi_core.cpp lib/midi_core/src/midi_core.cpp lib/midi_core/src/midi_core.h
	mkdir -p build
	c++ -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_midi_core.cpp lib/midi_core/src/midi_core.cpp

build:
	arduino-cli compile --fqbn $(FQBN) --build-property "tools.ctags.path=/opt/homebrew/bin" --library lib/midi_core --output-dir $(BUILD) $(SKETCH)

flash: build
	arduino-cli upload --fqbn $(FQBN) --input-dir $(BUILD) --port $(PORT) $(SKETCH)

clean:
	rm -rf build
