PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*))
ESP32_CORE ?= $(shell arduino-cli config get directories.data)/packages/esp32/hardware/esp32/3.3.12
# Arduino's bundled ctags is x86-only on Apple Silicon; CI sets /usr/bin.
CTAGS_DIR ?= /opt/homebrew/bin

# Board x app set (docs/architecture.md, "App sets and builds").
# `make build BOARD=waveshare_35 APPS=hub-loader`; `make site` builds them all.
# The boards are the profiles in firmware/boards.json (docs/boards.md, "Adding a
# board"); every board builds the one sketch, with its board_config.h.
# BOARDS are the tested ones: CI, make site and the installer build only those.
# Any profile builds and probes by name (ALL_BOARDS).
BOARDS := $(shell python3 -I tools/boards.py ids)
ALL_BOARDS := $(shell python3 -I tools/boards.py all-ids)
ifeq ($(BOARDS),)
$(error firmware/boards.json has a problem: python3 -I tools/boards.py check says what)
endif
APP_SETS := hub hub-loader everything
# Development app sets: CI builds them (its synth job), make site never stages
# them, so testers never get them. synth is Big-S3 only (the ES8311 speaker).
DEV_APP_SETS := synth
# littlefs-python for the store image and the installer's store template (rule below).
TOOLS_PY := build/tools-venv/bin/python
BOARD ?= waveshare_169
# Recursive (=): only build and flash read it, so other targets don't run python for it.
FQBN = $(shell python3 -I tools/boards.py fqbn $(BOARD))
APPS ?= hub
# Every app set carries the Web UI's read-only diagnostics (Wi-Fi, GET /status,
# /log, /crash, /coredump, /store), off until Setup's Remote diagnostics turns
# them on (web.h). `everything` starts with them on.
APP_FLAGS_hub := -DAPP_WEB_UI=1
APP_FLAGS_hub-loader := -DAPP_LOADER=1 -DAPP_WEB_UI=1
APP_FLAGS_everything := -DAPP_LOADER=1 -DAPP_RECORDER=1 -DAPP_WEB_UI=1
APP_FLAGS_synth = -DAPP_SYNTH=1 $(SYNTH_C_FLAGS)
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
# WIFI_BOOTLOOP=1 (with DIAG_WEB=1): the board restarts itself (esp_restart, as
# Back to MIDI does) 100 s after each boot, so tools/boot_probe.py can time Wi-Fi
# across many software restarts unattended. Never a release.
WRITE_FLAGS += $(if $(filter 1,$(WIFI_BOOTLOOP)),-DWIFI_BOOTLOOP=1)
# HEAP_ON_SCREEN=1: Setup's network line shows free internal and DMA RAM, for
# sizing screens on a board whose USB port is busy hosting. Never a release.
WRITE_FLAGS += $(if $(filter 1,$(HEAP_ON_SCREEN)),-DHEAP_ON_SCREEN=1)
# BENCH=1 (with DIAG_WEB=1, APPS=hub-loader): the test bench's control API at
# the board's own address (small-v2.local ...; lib/web_core/src/bench.h), answered only on the boards in
# the local device registry (tools/devices.py), which a bench build needs.
# tools/bench.py drives it. Never a release: make site checks no shipped build has it.
ifeq ($(BENCH),1)
$(if $(filter 1,$(DIAG_WEB)),,$(error BENCH=1 needs DIAG_WEB=1))
WRITE_FLAGS += -DBENCH=1
endif
APP_LIBS_hub := --library lib/web_core
APP_LIBS_hub-loader := --library lib/fm1_loader --library lib/web_core
APP_LIBS_everything := --library lib/fm1_loader --library lib/web_core
APP_LIBS_synth := --library lib/synth
# DIAG_WEB=1: a build that starts with Remote diagnostics on, for Chad's lab
# boards and loader checkpoints (Setup still turns it off).
ifeq ($(DIAG_WEB),1)
WRITE_FLAGS += -DDIAG_DEFAULT_ON=1
BUILD_SUFFIX := -web
endif
ifeq ($(SOAK),1)
BUILD_SUFFIX := $(BUILD_SUFFIX)-soak
endif
ifeq ($(HEAP_ON_SCREEN),1)
BUILD_SUFFIX := $(BUILD_SUFFIX)-heap
endif
ifeq ($(WIFI_BOOTLOOP),1)
BUILD_SUFFIX := $(BUILD_SUFFIX)-bootloop
endif
ifeq ($(BENCH),1)
BUILD_SUFFIX := $(BUILD_SUFFIX)-bench
endif
# The sketch is still named for the first board; it's every board's. Renaming
# it waits for the branches editing it to land.
SKETCH := firmware/waveshare_169
BUILD := build/$(BOARD)/$(APPS)$(BUILD_SUFFIX)
# The board table (bench_boards.h, lib/web_core/src/bench.cpp): the registry's
# names and MACs for DIAG_WEB builds (required for BENCH=1), empty otherwise, so
# release builds carry no MACs. One per build, outside the repo.
BENCH_GEN = $(CACHE_ROOT)/bench-boards/$(APPS)$(BUILD_SUFFIX)
BENCH_HEADER_ARGS := $(if $(filter 1,$(DIAG_WEB)),--registry $(if $(filter 1,$(BENCH)),,--optional))
# arduino-cli's build paths and ui-shots' LVGL objects. make check points it
# outside its clone, so a check recompiles only what the commit changed.
CACHE_ROOT ?= $(CURDIR)/build/cache
# Mac tests use the Mac's clang; CI sets clang++ too (GCC's -Wformat-truncation
# objects to the deliberate cut-off of screen lines).
TEST_CXX ?= c++
SITE_PORT ?= 8088
# The app0 slot of firmware/<board>/partitions.csv (3 MB); Arduino's own limit is
# the default table's 1.25 MB.
APP_SLOT := 3145728
STORE_OFFSET := 0x3D0000
# Shown on the computer-mode screen, and makes every commit a distinct build.
# make build writes them to $(VERSION_DIR)/fw_version.h, rewritten only when
# they change: as -D flags they changed arduino-cli's build options, and it
# recompiled everything for every commit.
FW_VERSION := 0x$(shell git rev-parse --short=7 HEAD)
FW_DIRTY := $(if $(shell git status --porcelain --untracked-files=no),1,0)
VERSION_DIR := $(CACHE_ROOT)/version
# tools/boards.py's generated headers: each board's board_config.h, and the
# screens ui-shots renders. Rewritten only when they change, like fw_version.h.
BOARD_GEN := $(CACHE_ROOT)/boards

.PHONY: check test ui-shots ui-mocks build build-all flash recovery flash-recovery probe synth-bench flash-synth-bench site serve clean fixtures golden store-image flash-store store-add check-latest deploy-site rollback-site

FM1_SRC := $(wildcard lib/fm1_loader/src/*.cpp)
FM1_HDR := $(wildcard lib/fm1_loader/src/*.h) $(wildcard tests/support/*.h)
FM1_TESTS := build/test_fm1_package build/test_fm1_library build/test_fm1_installer \
	build/test_fm1_softkey build/test_fm1_posix build/test_fm1_locked build/test_fm1_library_posix \
	build/test_fm1_receive

test: build/check_package build/test_fm1_interop build/test_touch_map build/test_panel build/test_board_blob build/test_profile_link test-board-configs build/test_midi_core build/test_status build/test_boot_mode build/test_broker build/test_describe build/test_web_core build/test_store_json build/test_loader_messages build/test_bench_writer build/test_ota $(FM1_TESTS) build/receive_pipe fixtures $(TOOLS_PY)
	./build/test_touch_map
	./build/test_panel
	./build/test_board_blob tests/boards/blobs.tsv
	./build/test_profile_link
	./build/test_midi_core
	./build/test_status
	./build/test_boot_mode
	./build/test_ota
	@# The recovery app's .ino stays comments only: Arduino rewrites .ino files (firmware/recovery/recovery.ino says how that broke it).
	@! grep -vqE '^[[:space:]]*(//.*)?$$' firmware/recovery/recovery.ino || { echo 'firmware/recovery/recovery.ino has code: move it to recovery_app.cpp'; exit 1; }
	./build/test_broker
	./build/test_describe
	./build/test_web_core
	./build/test_store_json build/store.json
	./build/test_loader_messages
	./build/test_bench_writer
	python3 -I -c "import json; json.load(open('build/store.json'))"
	for t in $(FM1_TESTS); do ./$$t || exit 1; done
	python3 -c "import shutil; shutil.rmtree('build/interop', ignore_errors=True)"
	python3 -I tools/fm1lib.py selftest build/interop tests/fixtures/skiff-1.4.fwsc \
		tests/fixtures/sloop-2.8.1.fwsc tests/fixtures/FM-1_015.fwsc
	./build/test_fm1_interop build/interop
	python3 -I tools/check_partitions.py $(SKETCH)/partitions.csv
	python3 -I tests/test_boards.py
	python3 -I tests/test_devices.py
	python3 -I tests/installer/test_tables.py
	$(TOOLS_PY) tests/installer/test_store_template.py
	node --test tests/installer/*.test.mjs
	node --test tests/functions/*.test.mjs
	python3 -I tests/test_deploy_site.py
	python3 -I tests/test_reports.py
	python3 -I tests/test_installs.py
	python3 -I tests/test_check.py
	python3 -I tests/test_ecosystem_check.py
	python3 -I tests/test_gen_families.py
	python3 -I tests/test_bench.py
	python3 -I tests/test_bench_recover.py
	python3 -I tests/test_guide.py

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

# The bench's sector writer (BENCH=1 builds only) against the mock FM-1.
build/test_bench_writer: tests/test_bench_writer.cpp firmware/waveshare_169/bench_writer.cpp firmware/waveshare_169/bench_writer.h $(FM1_TEST_SRC) $(FM1_HDR)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -DFM1_LOADER_WRITES=1 -Ilib/fm1_loader/src -Ifirmware/waveshare_169 -Itests -o $@ $< firmware/waveshare_169/bench_writer.cpp $(FM1_TEST_SRC)

# The board for the page's add-firmware tests (tests/installer/add.test.mjs).
build/receive_pipe: tests/receive_pipe.cpp $(FM1_TEST_SRC) $(FM1_HDR)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/fm1_loader/src -Itests -o $@ $< $(FM1_TEST_SRC)

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

# The touch chip's portrait coordinates turned to the screen's (board.cpp).
build/test_touch_map: tests/test_touch_map.cpp firmware/waveshare_169/touch_map.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ifirmware/waveshare_169 -Ilib/board/src -o $@ $<

# The panel's MADCTL byte and the touch axes from a profile (board_config_types.h, touch_map.h).
build/test_panel: tests/test_panel.cpp lib/board/src/board_config_types.h firmware/waveshare_169/touch_map.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ifirmware/waveshare_169 -Ilib/board/src -o $@ $<

# Every profile's generated board_config.h compiles and makes sense (tests/test_board_config.cpp).
.PHONY: test-board-configs
test-board-configs: tests/test_board_config.cpp lib/board/src/board_config_types.h firmware/boards.json tools/boards.py
	for b in $(ALL_BOARDS); do \
		python3 -I tools/boards.py header $$b build/board-configs/$$b/board_config.h || exit 1; \
		$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ibuild/board-configs/$$b -Ifirmware/waveshare_169 -Ilib/board/src -Ilib/host_ui/src \
			-o build/board-configs/$$b/test $< && ./build/board-configs/$$b/test || exit 1; done

# The board profile on the wire (lib/board): the cases boards.py generates, and the Computer-mode commands.
BOARD_LIB := lib/board/src/board_blob.cpp lib/board/src/board_blob.h lib/board/src/board_config_types.h lib/host_ui/src/board_profile.h
build/test_board_blob: tests/test_board_blob.cpp $(BOARD_LIB) tests/boards/blobs.tsv
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/board/src -Ilib/host_ui/src -o $@ $< lib/board/src/board_blob.cpp
build/test_profile_link: tests/test_profile_link.cpp lib/board/src/profile_link.cpp lib/board/src/profile_link.h $(BOARD_LIB)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/board/src -Ilib/host_ui/src -o $@ $< lib/board/src/profile_link.cpp lib/board/src/board_blob.cpp

build/test_midi_core: tests/test_midi_core.cpp lib/midi_core/src/midi_core.cpp lib/midi_core/src/midi_core.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_midi_core.cpp lib/midi_core/src/midi_core.cpp

build/test_status: tests/test_status.cpp lib/midi_core/src/status.cpp lib/midi_core/src/status.h lib/midi_core/src/midi_core.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_status.cpp lib/midi_core/src/status.cpp lib/midi_core/src/midi_core.cpp

build/test_boot_mode: tests/test_boot_mode.cpp lib/midi_core/src/boot_mode.cpp lib/midi_core/src/boot_mode.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/midi_core/src -o $@ tests/test_boot_mode.cpp lib/midi_core/src/boot_mode.cpp

build/test_ota: tests/test_ota.cpp tests/support/ota_sim.h lib/ota/src/ota.cpp lib/ota/src/ota.h lib/fm1_loader/src/crypto.cpp
	mkdir -p build
	$(TEST_CXX) -std=c++17 -O1 -Wall -Wextra -Werror -Ilib/ota/src -Ilib/fm1_loader/src -Itests -o $@ tests/test_ota.cpp lib/ota/src/ota.cpp lib/fm1_loader/src/crypto.cpp

build/test_broker: tests/test_broker.cpp lib/platform/src/broker.cpp lib/platform/src/broker.h lib/midi_core/src/midi_core.cpp lib/midi_core/src/midi_core.h
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/platform/src -Ilib/midi_core/src -o $@ tests/test_broker.cpp lib/platform/src/broker.cpp lib/midi_core/src/midi_core.cpp

build/test_describe: tests/test_describe.cpp lib/platform/src/describe.cpp lib/platform/src/describe.h lib/platform/src/broker.cpp lib/platform/src/broker.h lib/midi_core/src/midi_core.cpp
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -Ilib/platform/src -Ilib/midi_core/src -o $@ tests/test_describe.cpp lib/platform/src/describe.cpp lib/platform/src/broker.cpp lib/midi_core/src/midi_core.cpp

WEB_CORE_SRC := $(wildcard lib/web_core/src/*.cpp)
# The Mac tests' board table: tools/devices.example.json's made-up MACs.
EXAMPLE_BOARDS := build/example-boards/bench_boards.h
$(EXAMPLE_BOARDS): tools/devices.example.json tools/devices.py
	python3 -I tools/devices.py header $@ --registry tools/devices.example.json
build/test_web_core: tests/test_web_core.cpp $(WEB_CORE_SRC) $(wildcard lib/web_core/src/*.h) $(EXAMPLE_BOARDS)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -pthread -Ilib/web_core/src -I$(dir $(EXAMPLE_BOARDS)) -o $@ tests/test_web_core.cpp $(WEB_CORE_SRC)

build/test_store_json: tests/test_store_json.cpp firmware/waveshare_169/store_json.cpp firmware/waveshare_169/store_json.h $(FM1_TEST_SRC) $(FM1_HDR) $(WEB_CORE_SRC) $(EXAMPLE_BOARDS)
	mkdir -p build
	$(TEST_CXX) -std=c++17 -Wall -Wextra -Werror -DAPP_LOADER=1 -DAPP_WEB_UI=1 -Ilib/fm1_loader/src -Ilib/web_core/src -I$(dir $(EXAMPLE_BOARDS)) \
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
LVGL_OBJ := $(patsubst $(LVGL_DIR)/src/%.c,$(CACHE_ROOT)/lvgl-obj/%.o,$(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null))
UI_FLAGS := -DLV_CONF_INCLUDE_SIMPLE -Ifirmware/config -I$(LVGL_DIR) -I$(LVGL_DIR)/src -Itests/ui_shots/stub
$(CACHE_ROOT)/lvgl-obj/%.o: $(LVGL_DIR)/src/%.c firmware/config/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) -O1 $(UI_FLAGS) -c $< -o $@
# The shots use the firmware's own wording: Play's statuses (status.cpp) and
# the FM-1 pages' (loader_messages.cpp, with fm1_loader).
UI_SHOTS_SRC := tests/ui_shots/ui_shots.cpp lib/host_ui/src/host_ui.cpp lib/midi_core/src/status.cpp \
	lib/midi_core/src/midi_core.cpp firmware/waveshare_169/loader_messages.cpp $(FM1_SRC)
# Every board's screen comes from its profile (firmware/boards.json).
$(BOARD_GEN)/board_shots.h: firmware/boards.json tools/boards.py
	python3 -I tools/boards.py shots $@
	touch $@
build/ui_shots: $(UI_SHOTS_SRC) $(wildcard tests/ui_shots/*.h tests/ui_shots/stub/*.h lib/host_ui/src/*.h lib/midi_core/src/*.h) \
		firmware/waveshare_169/loader_messages.h $(FM1_HDR) $(LVGL_OBJ) $(BOARD_GEN)/board_shots.h
	@echo "$(TEST_CXX) ... -o $@"
	@$(TEST_CXX) -std=c++17 -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations $(UI_FLAGS) -DAPP_LOADER=1 -I$(BOARD_GEN) \
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
	$(if $(filter $(APPS),$(APP_SETS) $(DEV_APP_SETS)),,$(error APPS=$(APPS); known: $(APP_SETS) $(DEV_APP_SETS)))
	$(if $(filter $(BOARD),$(ALL_BOARDS)),,$(error BOARD=$(BOARD); known: $(ALL_BOARDS)))
	$(if $(filter synth,$(APPS)),$(if $(filter $(SYNTH_BOARD),$(BOARD)),,$(error APPS=synth builds for BOARD=$(SYNTH_BOARD) only)))
	python3 -I tools/boards.py header $(BOARD) $(BOARD_GEN)/$(BOARD)/board_config.h
	python3 -I tools/devices.py header $(BENCH_GEN)/bench_boards.h $(BENCH_HEADER_ARGS)
	mkdir -p $(VERSION_DIR)
	printf '#define FW_VERSION %s\n#define FW_DIRTY %s\n' $(FW_VERSION) $(FW_DIRTY) > $(VERSION_DIR)/fw_version.h.$$$$ \
		&& { cmp -s $(VERSION_DIR)/fw_version.h.$$$$ $(VERSION_DIR)/fw_version.h \
			&& rm $(VERSION_DIR)/fw_version.h.$$$$ || mv $(VERSION_DIR)/fw_version.h.$$$$ $(VERSION_DIR)/fw_version.h; }
	arduino-cli compile --fqbn $(FQBN) --build-property "tools.ctags.path=$(CTAGS_DIR)" \
		--build-property "upload.maximum_size=$(APP_SLOT)" \
		--build-property "compiler.cpp.extra_flags=-I$(VERSION_DIR) -I$(BOARD_GEN)/$(BOARD) -I$(BENCH_GEN) -DESP_USB_HOST_MAX_SERIAL_PORTS=0 $(APP_FLAGS_$(APPS)) $(WRITE_FLAGS) -DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config" \
		--build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$(CURDIR)/firmware/config $(if $(filter synth,$(APPS)),$(SYNTH_C_FLAGS))" \
		--library lib/midi_core --library lib/EspUsbHost --library lib/host_ui --library lib/board --library lib/platform --library lib/ota $(APP_LIBS_$(APPS)) \
		--build-path $(CACHE_ROOT)/$(BOARD)/$(APPS)$(BUILD_SUFFIX) --output-dir $(BUILD) $(SKETCH)

build-all:
	for b in $(BOARDS); do for a in $(APP_SETS); do $(MAKE) build BOARD=$$b APPS=$$a || exit 1; done; done

flash: build
	arduino-cli upload --fqbn $(FQBN) --input-dir $(BUILD) --port $(PORT) $(SKETCH)

# The recovery app for the `recovery` slot (firmware/recovery, docs/ota-updates.md):
# lib/ota's copy and rollback, nothing else. `make recovery` builds it;
# `make flash-recovery` writes that slot alone over USB, so app0, NVS and the
# store are untouched (board in download mode).
RECOVERY_BUILD := build/$(BOARD)/recovery
RECOVERY_OFFSET := 0x310000
RECOVERY_SLOT := 786432
recovery:
	$(if $(filter $(BOARD),$(ALL_BOARDS)),,$(error BOARD=$(BOARD); known: $(ALL_BOARDS)))
	arduino-cli compile --fqbn $(FQBN) --build-property "tools.ctags.path=$(CTAGS_DIR)" \
		--build-property "upload.maximum_size=$(RECOVERY_SLOT)" \
		--library lib/ota --build-path $(CACHE_ROOT)/$(BOARD)/recovery --output-dir $(RECOVERY_BUILD) firmware/recovery
flash-recovery: recovery
	$(firstword $(wildcard $(ESP32_CORE)/../../../tools/esptool_py/*/esptool)) --chip esp32s3 --port $(PORT) \
		--after watchdog-reset write-flash $(RECOVERY_OFFSET) $(RECOVERY_BUILD)/recovery.ino.bin

# A board profile's first check, on the board (firmware/board_probe/probe.cpp):
# the same drivers, with the USB port left a serial device so PROBE_LOG shows
# what it finds. `make probe BOARD=waveshare_35`, board in download mode.
PROBE_BUILD := build/$(BOARD)/probe
probe:
	$(if $(filter $(BOARD),$(ALL_BOARDS)),,$(error BOARD=$(BOARD); known: $(ALL_BOARDS)))
	python3 -I tools/boards.py header $(BOARD) $(BOARD_GEN)/$(BOARD)/board_config.h
	arduino-cli compile --fqbn $(subst USBMode=default,USBMode=hwcdc,$(FQBN)),CDCOnBoot=cdc --build-property "tools.ctags.path=$(CTAGS_DIR)" \
		--build-property "compiler.cpp.extra_flags=-I$(BOARD_GEN)/$(BOARD) -I$(CURDIR)/firmware/waveshare_169 -I$(CURDIR)/lib/board/src -I$(CURDIR)/lib/host_ui/src" \
		--library lib/board --build-path $(CACHE_ROOT)/$(BOARD)/probe --output-dir $(PROBE_BUILD) firmware/board_probe
	arduino-cli upload --fqbn $(subst USBMode=default,USBMode=hwcdc,$(FQBN)),CDCOnBoot=cdc --input-dir $(PROBE_BUILD) --port $(PORT) firmware/board_probe

# Outrigger Synth's bench for Big-S3 (firmware/synth_bench/bench.cpp): the board's
# drivers and lib/synth's audio out, with the USB port left a serial device so
# the Mac reads the numbers. Never staged into the site. `make synth-bench` builds;
# `make flash-synth-bench` also uploads (board in download mode).
SYNTH_BOARD := waveshare_35
# Felucca's generated tables, which its sources include by bare name.
SYNTH_C_FLAGS := -I$(CURDIR)/lib/synth/third_party/fm1-cartridge/third_party/felucca/generated
SYNTH_BENCH_BUILD := build/$(SYNTH_BOARD)/synth-bench
SYNTH_BENCH_FQBN = $(subst USBMode=default,USBMode=hwcdc,$(shell python3 -I tools/boards.py fqbn $(SYNTH_BOARD))),CDCOnBoot=cdc
synth-bench:
	python3 -I tools/boards.py header $(SYNTH_BOARD) $(BOARD_GEN)/$(SYNTH_BOARD)/board_config.h
	arduino-cli compile --fqbn $(SYNTH_BENCH_FQBN) --build-property "tools.ctags.path=$(CTAGS_DIR)" \
		--build-property "compiler.cpp.extra_flags=-DAPP_SYNTH=1 $(SYNTH_C_FLAGS) -I$(BOARD_GEN)/$(SYNTH_BOARD) -I$(CURDIR)/firmware/waveshare_169 -I$(CURDIR)/lib/board/src -I$(CURDIR)/lib/host_ui/src" \
		--build-property "compiler.c.extra_flags=$(SYNTH_C_FLAGS)" \
		--library lib/board --library lib/synth --build-path $(CACHE_ROOT)/$(SYNTH_BOARD)/synth-bench --output-dir $(SYNTH_BENCH_BUILD) firmware/synth_bench
flash-synth-bench: synth-bench
	arduino-cli upload --fqbn $(SYNTH_BENCH_FQBN) --input-dir $(SYNTH_BENCH_BUILD) --port $(PORT) firmware/synth_bench

# The store partition's first setup: a LittleFS image holding JieLi's loader
# (from tests/fixtures). JieLi's code, so it stays in build/ and is never staged
# into site/. Flashing it ERASES every backup and save in the store, so it asks
# for CONFIRM=erase-store. The board must be in download mode (Computer mode).
$(TOOLS_PY):
	python3 -m venv build/tools-venv
	$(TOOLS_PY) -m pip install -q littlefs-python==0.19.0 pyserial==3.5
store-image: $(TOOLS_PY) fixtures
	$(TOOLS_PY) tools/store_image.py $(SKETCH)/partitions.csv build/store/store.bin
flash-store: store-image
	$(if $(filter erase-store,$(CONFIRM)),,$(error flash-store erases every backup in the store: add CONFIRM=erase-store))
	$(firstword $(wildcard $(ESP32_CORE)/../../../tools/esptool_py/*/esptool)) --chip esp32s3 --port $(PORT) \
		write-flash $(STORE_OFFSET) build/store/store.bin

# Adds packages to the board's store without losing its backups and saves:
# reads the store back (kept in build/store/), adds, checks nothing else changed,
# writes it back. Board in Computer mode. make store-add PKGS="a.fwsc b.fwsc" [PIN=1]
store-add: $(TOOLS_PY)
	$(if $(PKGS)$(LIST),,$(error PKGS="file.fwsc ..." names the packages to add; LIST=1 only lists))
	$(TOOLS_PY) tools/store_add.py $(SKETCH)/partitions.csv $(PORT) $(if $(PIN),--pin) $(if $(LIST),--list) $(PKGS)

# The production deploy's check (docs/publishing.md): the worktree's last commit, checked out
# clean in one clone every worktree shares (~/Library/Caches/outrigger-check),
# one check at a time, then make test, make ui-shots and make site (every board
# x app set built and staged, with build_site.py's wl82loader.bin scan). Run it
# on the Mac; make deploy-site runs it first. tools/check.py says how; a pass
# leaves the site and its commit in build/checked/ for make deploy-site.
CHECK_HOME ?= $(HOME)/Library/Caches/outrigger-check
check:
	python3 -I tools/check.py --home $(CHECK_HOME)

# Stage the firmware into site/ for the browser installer (site/installer/).
# fixtures brings wl82loader.bin, so build_site can scan for pieces of it.
# NOTICE and LICENSE go beside the firmware: GPL-3.0 and the MIT and BSD
# licenses of what it includes ask for their texts to travel with the binaries.
# Loader app sets also get the store template (tools/store_template.py).
site: build-all fixtures $(TOOLS_PY)
	cp NOTICE site/NOTICE.txt
	cp LICENSE site/LICENSE.txt
	for b in $(BOARDS); do \
		$(TOOLS_PY) tools/store_template.py $(SKETCH)/partitions.csv build/$$b/store-template.json || exit 1; \
		for a in $(APP_SETS); do \
		if grep -q /bench/ build/$$b/$$a/*.ino.bin; then echo "build/$$b/$$a has the bench API"; exit 1; fi; \
		python3 tools/build_site.py $$b $$a build/$$b/$$a $(ESP32_CORE)/tools/partitions/boot_app0.bin \
			$(shell git describe --always --dirty) build/$$b/store-template.json || exit 1; done; done

# Web Serial needs a secure context; http://localhost counts.
serve: site
	python3 -m http.server $(SITE_PORT) --bind 127.0.0.1 --directory site

# Publish the site on Cloudflare Pages: CHANNEL=beta (beta.outrigger.music, any
# pushed commit) or CHANNEL=production (outrigger.music, origin/master only).
# Uploads CI's build of this commit (its push run's site-firmware), or, for FULL=1, a
# batch touching the critical list since production's commit, or a commit CI never built itself,
# the site make check builds here (build/checked/site). docs/publishing.md.
CHANNEL ?= beta
deploy-site:
	@src=$$(python3 -I tools/deploy_site.py source $(CHANNEL) $(FULL)) || exit 1; \
	if [ "$$src" = ci ]; then python3 -I tools/deploy_site.py deploy-ci $(CHANNEL); \
	elif [ "$$src" = beta ]; then python3 -I tools/deploy_site.py deploy-beta $(CHANNEL); \
	else $(MAKE) check && python3 -I tools/deploy_site.py deploy $(CHANNEL) build/checked/site; fi

# Put the deploy before the live one back on CHANNEL (or COMMIT=<sha>'s), in seconds,
# without make check: production through Cloudflare's rollback, beta from the kept copy.
rollback-site:
	python3 -I tools/deploy_site.py rollback $(CHANNEL) $(COMMIT)

clean:
	rm -rf build
