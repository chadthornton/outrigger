# huge_app (3 MB app, nvs still at 0x9000) is spike-only: Wi-Fi doesn't fit the
# default 1.25 MB app slot. The real layout is the loader plan's decision 5.
FQBN := esp32:esp32:esp32s3:USBMode=default,FlashSize=16M,PSRAM=opi,PartitionScheme=huge_app
SKETCH := firmware/waveshare_169
BUILD := build/waveshare_169
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*))

.PHONY: test build flash clean

test: build/test_midi_core build/test_status build/test_web_core
	./build/test_midi_core
	./build/test_status
	./build/test_web_core

build/test_midi_core: tests/test_midi_core.cpp lib/midi_core/src/midi_core.cpp lib/midi_core/src/midi_core.h
	mkdir -p build
	c++ -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_midi_core.cpp lib/midi_core/src/midi_core.cpp

build/test_status: tests/test_status.cpp lib/midi_core/src/status.cpp lib/midi_core/src/status.h lib/midi_core/src/midi_core.h
	mkdir -p build
	c++ -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_status.cpp lib/midi_core/src/status.cpp lib/midi_core/src/midi_core.cpp

WEB_CORE := $(wildcard lib/web_core/src/*.cpp) $(wildcard lib/web_core/src/*.h)
build/test_web_core: tests/test_web_core.cpp $(WEB_CORE)
	mkdir -p build
	c++ -std=c++17 -Wall -Wextra -Werror -Ilib/web_core/src -o $@ tests/test_web_core.cpp $(filter %.cpp,$(WEB_CORE))

WEB_PAGES := web/index.html web/setup.html
build/web/web_assets.h: tools/embed_web.py $(WEB_PAGES)
	python3 -I tools/embed_web.py $@ $(WEB_PAGES)

build: build/web/web_assets.h
	arduino-cli compile --fqbn $(FQBN) --build-property "tools.ctags.path=/opt/homebrew/bin" \
		--build-property "compiler.cpp.extra_flags=-DESP_USB_HOST_MAX_SERIAL_PORTS=0 -DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config -I$(CURDIR)/build/web" \
		--build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--library lib/midi_core --library lib/EspUsbHost --library lib/host_ui --library lib/web_core --output-dir $(BUILD) $(SKETCH)

flash: build
	arduino-cli upload --fqbn $(FQBN) --input-dir $(BUILD) --port $(PORT) $(SKETCH)

clean:
	rm -rf build
