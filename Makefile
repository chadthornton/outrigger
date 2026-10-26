FQBN := esp32:esp32:esp32s3:USBMode=default,FlashSize=16M,PSRAM=opi
SKETCH := firmware/waveshare_169
BUILD := build/waveshare_169
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*))
ESP32_CORE := $(HOME)/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12
SITE_PORT ?= 8088

.PHONY: test build flash site serve clean

test: build/test_midi_core build/test_status build/test_boot_mode
	./build/test_midi_core
	./build/test_status
	./build/test_boot_mode

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
		--build-property "compiler.cpp.extra_flags=-DESP_USB_HOST_MAX_SERIAL_PORTS=0 -DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--library lib/midi_core --library lib/EspUsbHost --library lib/host_ui --output-dir $(BUILD) $(SKETCH)

flash: build
	arduino-cli upload --fqbn $(FQBN) --input-dir $(BUILD) --port $(PORT) $(SKETCH)

# Stage the firmware into site/ for the browser installer (ESP Web Tools).
site: build
	python3 tools/build_site.py waveshare-esp32-s3-touch-lcd-1.69 $(BUILD) $(ESP32_CORE)/tools/partitions/boot_app0.bin \
		$(shell git describe --always --dirty)

# Web Serial needs a secure context; http://localhost counts.
serve: site
	python3 -m http.server $(SITE_PORT) --bind 127.0.0.1 --directory site

clean:
	rm -rf build
