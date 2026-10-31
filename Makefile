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
# hub-loader ships with writes on (checkpoints 2 and 3 passed, and the USB host
# crash at the FM-1's disconnect is fixed); WRITES=0 builds it read-only.
# `everything` stays read-only until its own checkpoint.
WRITES_DEFAULT_hub-loader := 1
WRITES ?= $(or $(WRITES_DEFAULT_$(APPS)),0)
WRITE_FLAGS := $(if $(filter 1,$(WRITES)),-DFM1_LOADER_WRITES=1) $(if $(filter 1,$(DIAG)),-DLOADER_DIAG=1)
# SOAK=1 (with DIAG_WEB=1): the board backs up the FM-1 by itself, over and over,
# for hunting USB host crashes unattended (loader_app.cpp, LOADER_SOAK). Never a release.
WRITE_FLAGS += $(if $(filter 1,$(SOAK)),-DLOADER_SOAK=1)
APP_LIBS_hub :=
APP_LIBS_hub-loader := --library lib/fm1_loader
APP_LIBS_everything := --library lib/fm1_loader --library lib/web_core
# DIAG_WEB=1 adds the Web UI's read-only diagnostics (Wi-Fi, GET /status, /log,
# /crash, /coredump, /store) to hub-loader, which the board runs for loader
# checkpoints. `everything` always has them.
ifeq ($(DIAG_WEB),1)
ifneq ($(APPS),everything)
APP_FLAGS_$(APPS) += -DAPP_WEB_UI=1
APP_LIBS_$(APPS) += --library lib/web_core
BUILD_SUFFIX := -web
endif
endif
ifeq ($(SOAK),1)
BUILD_SUFFIX := $(BUILD_SUFFIX)-soak
endif
SKETCH := firmware/$(BOARD)
BUILD := build/$(BOARD)/$(APPS)$(BUILD_SUFFIX)
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

.PHONY: check test ui-shots ui-mocks build build-all flash site serve clean fixtures golden store-image flash-store store-add check-latest deploy-site

FM1_SRC := $(wildcard lib/fm1_loader/src/*.cpp)
FM1_HDR := $(wildcard lib/fm1_loader/src/*.h) $(wildcard tests/support/*.h)
FM1_TESTS := build/test_fm1_package build/test_fm1_library build/test_fm1_installer \
	build/test_fm1_softkey build/test_fm1_posix build/test_fm1_locked build/test_fm1_library_posix

test: build/check_package build/test_fm1_interop build/test_midi_core build/test_status build/test_boot_mode build/test_broker build/test_describe build/test_web_core build/test_store_json build/test_loader_messages $(FM1_TESTS) fixtures
	./build/test_midi_core
	./build/test_status
	./build/test_boot_mode
	./build/test_broker
	./build/test_describe
	./build/test_web_core
	./build/test_store_json build/store.json
	./build/test_loader_messages
	python3 -I -c "import json; json.load(open('build/store.json'))"
	for t in $(FM1_TESTS); do ./$$t || exit 1; done
	python3 -c "import shutil; shutil.rmtree('build/interop', ignore_errors=True)"
	python3 -I tools/fm1lib.py selftest build/interop tests/fixtures/skiff-1.4.fwsc \
		tests/fixtures/sloop-2.8.1.fwsc tests/fixtures/FM-1_015.fwsc
	./build/test_fm1_interop build/interop
	python3 -I tools/check_partitions.py firmware/$(BOARD)/partitions.csv
	python3 -I tests/test_deploy_site.py

# The Mac tests may write the mock FM-1 (FM1_LOADER_WRITES=1); test_fm1_locked
# is built without it, as every firmware build is until hardware checkpoint 3.
FM1_TEST_SRC := $(FM1_SRC) $(wildcard tests/support/*.cpp)
build/test_fm1_locked: tests/test_fm1_locked.cpp $(FM1_TEST_SRC) $(FM1_HDR)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/fm1_loader/src -Itests -o $@ $< $(FM1_TEST_SRC)

build/test_fm1_library_posix: tests/test_fm1_library.cpp $(FM1_TEST_SRC) $(FM1_HDR)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -DFM1_LOADER_WRITES=1 -DTEST_POSIX_STORAGE -Ilib/fm1_loader/src -Itests -o $@ $< $(FM1_TEST_SRC)

# test_fm1_installer also drives a switch's chain (firmware/waveshare_169/switch_chain.h).
build/test_fm1_%: tests/test_fm1_%.cpp $(FM1_TEST_SRC) $(FM1_HDR) firmware/waveshare_169/switch_chain.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -DFM1_LOADER_WRITES=1 -Ilib/fm1_loader/src -Ifirmware/waveshare_169 -Itests -o $@ $< $(FM1_TEST_SRC)

# Packages and the loader blob for the fm1_loader tests, pinned by SHA-256 and
# kept out of git (tests/fixtures/).
fixtures:
	python3 -I tools/fetch_fixtures.py tests/fixtures

# The newest release of each firmware the tests pin (or every catalog firmware,
# LATEST_ALL=1), fetched into build/latest/ and run through a mock install by
# build/check_package. Not part of make test: the catalog moves on its own.
build/check_package: tests/check_package.cpp $(FM1_TEST_SRC) $(FM1_HDR)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -DFM1_LOADER_WRITES=1 -Ilib/fm1_loader/src -Itests -o $@ $< $(FM1_TEST_SRC)

check-latest: build/check_package fixtures
	python3 -I tools/check_latest.py build/latest $(if $(filter 1,$(LATEST_ALL)),--all)

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

WEB_CORE_SRC := $(wildcard lib/web_core/src/*.cpp)
build/test_web_core: tests/test_web_core.cpp $(WEB_CORE_SRC) $(wildcard lib/web_core/src/*.h)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -pthread -Ilib/web_core/src -o $@ tests/test_web_core.cpp $(WEB_CORE_SRC)

build/test_store_json: tests/test_store_json.cpp firmware/waveshare_169/store_json.cpp firmware/waveshare_169/store_json.h $(FM1_TEST_SRC) $(FM1_HDR) $(WEB_CORE_SRC)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -DAPP_LOADER=1 -DAPP_WEB_UI=1 -Ilib/fm1_loader/src -Ilib/web_core/src \
		-Ilib/platform/src -Ifirmware/waveshare_169 -Itests -o $@ $< firmware/waveshare_169/store_json.cpp $(FM1_TEST_SRC) $(WEB_CORE_SRC)

# The FM-1 pages' wording (firmware/waveshare_169/loader_messages.cpp): pure C++
# beside loader_app.cpp, which needs Arduino and FreeRTOS and can't run here.
build/test_loader_messages: tests/test_loader_messages.cpp firmware/waveshare_169/loader_messages.cpp firmware/waveshare_169/loader_messages.h lib/host_ui/src/host_ui.h $(FM1_TEST_SRC) $(FM1_HDR)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -DAPP_LOADER=1 -Ilib/fm1_loader/src -Ilib/host_ui/src -Ilib/midi_core/src \
		-Ilib/platform/src -Ifirmware/waveshare_169 -Itests -o $@ $< firmware/waveshare_169/loader_messages.cpp $(FM1_TEST_SRC)

# Renders every host_ui screen with LVGL itself, on the Mac or in CI, writes
# PNGs and a contact sheet to build/ui-shots/, and fails on layout problems
# (tests/ui_shots/ui_shots.cpp). It uses the Arduino library's LVGL; CI clones
# the same pinned tag into build/lvgl and sets LVGL_DIR.
LVGL_VERSION := 9.6.0
LVGL_DIR ?= $(HOME)/Documents/Arduino/libraries/lvgl
ifneq ($(filter ui-shots build/ui_shots ui-mocks build/ui_mocks,$(MAKECMDGOALS)),)
ifeq ($(shell grep -qx 'version=$(LVGL_VERSION)' $(LVGL_DIR)/library.properties 2>/dev/null && echo ok),)
$(error ui-shots needs LVGL $(LVGL_VERSION) in LVGL_DIR=$(LVGL_DIR): git clone --depth 1 --branch v$(LVGL_VERSION) https://github.com/lvgl/lvgl build/lvgl, then add LVGL_DIR=build/lvgl)
endif
endif
LVGL_OBJ := $(patsubst $(LVGL_DIR)/src/%.c,build/lvgl-obj/%.o,$(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null))
UI_FLAGS := -DLV_CONF_INCLUDE_SIMPLE -Ifirmware/config -I$(LVGL_DIR) -I$(LVGL_DIR)/src -Itests/ui_shots/stub
build/lvgl-obj/%.o: $(LVGL_DIR)/src/%.c firmware/config/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) -O1 $(UI_FLAGS) -c $< -o $@
# The shots use the firmware's own wording: Play's statuses (status.cpp) and
# the FM-1 pages' (loader_messages.cpp, with fm1_loader).
UI_SHOTS_SRC := tests/ui_shots/ui_shots.cpp lib/host_ui/src/host_ui.cpp lib/midi_core/src/status.cpp \
	lib/midi_core/src/midi_core.cpp firmware/waveshare_169/loader_messages.cpp $(FM1_SRC)
build/ui_shots: $(UI_SHOTS_SRC) $(wildcard tests/ui_shots/*.h tests/ui_shots/stub/*.h lib/host_ui/src/*.h lib/midi_core/src/*.h) \
		firmware/waveshare_169/loader_messages.h $(FM1_HDR) $(LVGL_OBJ)
	@echo "$(TEST_CXX) ... -o $@"
	@$(TEST_CXX) -std=c++17 -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations $(UI_FLAGS) -DAPP_LOADER=1 \
		-Ilib/host_ui/src -Ilib/midi_core/src -Ilib/fm1_loader/src -Ilib/platform/src -Ifirmware/waveshare_169 \
		-o $@ $(UI_SHOTS_SRC) $(LVGL_OBJ) -lz
ui-shots: build/ui_shots
	rm -rf build/ui-shots
	./build/ui_shots build/ui-shots

# Design mockups for the UI redesign (docs/ui-redesign.md), rendered with the
# same LVGL and checks; nothing here ships.
build/ui_mocks: tests/ui_mocks/ui_mocks.cpp tests/ui_shots/png.h $(LVGL_OBJ)
	@echo "$(TEST_CXX) ... -o $@"
	@$(TEST_CXX) -std=c++17 -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations $(UI_FLAGS) \
		-o $@ tests/ui_mocks/ui_mocks.cpp $(LVGL_OBJ) -lz
ui-mocks: build/ui_mocks
	rm -rf build/ui-mocks
	./build/ui_mocks build/ui-mocks

build:
	$(if $(filter $(APPS),$(APP_SETS)),,$(error APPS=$(APPS); known: $(APP_SETS)))
	arduino-cli compile --fqbn $(FQBN) --build-property "tools.ctags.path=$(CTAGS_DIR)" \
		--build-property "upload.maximum_size=$(APP_SLOT)" \
		--build-property "compiler.cpp.extra_flags=-DFW_VERSION=$(FW_VERSION) -DFW_DIRTY=$(FW_DIRTY) -DESP_USB_HOST_MAX_SERIAL_PORTS=0 $(APP_FLAGS_$(APPS)) $(WRITE_FLAGS) -DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--library lib/midi_core --library lib/EspUsbHost --library lib/host_ui --library lib/platform $(APP_LIBS_$(APPS)) \
		--build-path $(CURDIR)/build/cache/$(BOARD)/$(APPS)$(BUILD_SUFFIX) --output-dir $(BUILD) $(SKETCH)

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
	$(if $(PKGS)$(LIST),,$(error PKGS="file.fwsc ..." names the packages to add; LIST=1 only lists))
	$(TOOLS_PY) tools/store_add.py firmware/$(BOARD)/partitions.csv $(PORT) $(if $(PIN),--pin) $(if $(LIST),--list) $(PKGS)

# The merge gate's step 2 (README.md): the branch's last commit, cloned fresh
# into build/check so nothing uncommitted can help, then make test, make
# ui-shots and make site (every board x app set built and staged, with
# build_site.py's wl82loader.bin scan). Run it on the Mac and paste the summary
# into the PR. The fixtures are copied in (pinned by SHA-256, gitignored).
CHECK_DIR := build/check
check:
	rm -rf $(CHECK_DIR)
	git clone -q --shared . $(CHECK_DIR)
	git -C $(CHECK_DIR) checkout -q --detach $$(git rev-parse HEAD)
	mkdir -p $(CHECK_DIR)/tests/fixtures && cp tests/fixtures/* $(CHECK_DIR)/tests/fixtures/
	$(MAKE) -C $(CHECK_DIR) test ui-shots site > $(CHECK_DIR).log 2>&1 \
		|| { tail -30 $(CHECK_DIR).log; echo "check FAILED at $$(git rev-parse --short HEAD); log: $(CHECK_DIR).log"; exit 1; }
	@grep -E "passed|keeps the install|Sketch uses" $(CHECK_DIR).log
	@echo "check passed at $$(git rev-parse --short HEAD) (clean clone; log: $(CHECK_DIR).log)"

# Stage the firmware into site/ for the browser installer (ESP Web Tools).
# fixtures brings wl82loader.bin, so build_site can scan for pieces of it.
site: build-all fixtures
	for b in $(BOARDS); do for a in $(APP_SETS); do \
		python3 tools/build_site.py $$b $$a build/$$b/$$a $(ESP32_CORE)/tools/partitions/boot_app0.bin \
			$(shell git describe --always --dirty) || exit 1; done; done

# Web Serial needs a secure context; http://localhost counts.
serve: site
	python3 -m http.server $(SITE_PORT) --bind 127.0.0.1 --directory site

# Publish the site on Cloudflare Pages: CHANNEL=beta (beta.outrigger.music, any
# pushed commit) or CHANNEL=production (outrigger.music, origin/master only).
# Uploads the site make check built in its clean clone. README, "Publishing the site".
CHANNEL ?= beta
deploy-site:
	python3 -I tools/deploy_site.py guard $(CHANNEL)
	$(MAKE) check
	python3 -I tools/deploy_site.py deploy $(CHANNEL) $(CHECK_DIR)/site

clean:
	rm -rf build
