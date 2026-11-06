ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to an installed ps5-payload-dev SDK)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ROOT := ..
OUT := $(ROOT)/build-ps5
GEN := $(OUT)/generated
ELF := $(OUT)/ArchiveFetch.elf
HTTP_DIR ?= $(ROOT)/build/_deps/web_http-src
JSON_DIR ?= $(ROOT)/build/_deps/web_json-src
ASSET_DIR ?= $(ROOT)/build/web/assets
PACKAGES := libcurl libxml-2.0 openssl
PS5_SOURCES := $(addprefix $(ROOT)/ps5/diag/,main.cpp diag_log.cpp media_shortcut.cpp http_service.cpp instance_handoff.cpp web_assets.cpp native_libraries.cpp api_routes.cpp ca_bundle.cpp thread_stack.cpp extract_runner.cpp)
PS5_HEADERS := $(wildcard $(ROOT)/ps5/diag/*.hpp) $(wildcard $(ROOT)/ps5/extract/*.hpp)
# The shared ArchiveFetch backend; api_routes.cpp includes src/web_application.hpp for WebApplication.
APP_SOURCES := $(addprefix $(ROOT)/src/,gui_model.cpp application.cpp catalog.cpp transfer.cpp credential_store.cpp gui_platform.cpp)
# Mozilla's CA list from curl.se, pinned by date and SHA-256 (https://curl.se/docs/caextract.html).
CA_BUNDLE_DATE := 2026-10-20
CA_BUNDLE_SHA256 := 796f1389bfea13c0ed78f663b765baaff54a508e5483786bd6ece73c5e3e3cbe
CA_BUNDLE := $(GEN)/cacert-$(CA_BUNDLE_DATE).pem
PS5_FLAGS := -DPS5_PAYLOAD=1 -DARCHIVEFETCH_PS5_IDENTITY=1 -DARCHIVEFETCH_THREAD_NAME=\"ArchFtch.elf\" -DDIAG_CA_BUNDLE=\"$(abspath $(CA_BUNDLE))\" -DDIAG_CA_BUNDLE_DATE=\"$(CA_BUNDLE_DATE)\" -I$(GEN) -I$(HTTP_DIR) -I$(JSON_DIR)/include -I$(ROOT)/src -I$(ROOT)/ps5/extract
CXXFLAGS += -std=c++17 -O2 -Wall -Wextra
ARCHIVEFETCH_VERSION := $(strip $(file < $(ROOT)/VERSION))
CXXFLAGS += -DARCHIVEFETCH_VERSION=\"$(ARCHIVEFETCH_VERSION)\"

.PHONY: all check-deps
all: $(ELF)

# The archive extractor runs as its own short-lived process (started through the console's ELF loader), so
# decompression never runs inside ArchiveFetch. Single-threaded; ArchiveFetch carries it as embedded bytes.
EXTRACT_DIR := $(ROOT)/ps5/extract
EXTRACTOR := $(OUT)/ArchFtchX.elf
$(EXTRACTOR): $(EXTRACT_DIR)/extract_main.cpp $(EXTRACT_DIR)/extract_engine.cpp $(PS5_HEADERS) $(ROOT)/ps5/diag/diag_log.cpp $(ROOT)/src/remove_tree.hpp $(JSON_DIR)/include/nlohmann/json.hpp $(ROOT)/VERSION Makefile
	mkdir -p $(OUT)
	$(CXX) -DPS5_PAYLOAD=1 -I$(ROOT)/ps5/diag -I$(EXTRACT_DIR) -I$(ROOT)/src -I$(JSON_DIR)/include $(shell $(PKG_CONFIG) --cflags --static libarchive) $(CXXFLAGS) -o $@ $(EXTRACT_DIR)/extract_main.cpp $(EXTRACT_DIR)/extract_engine.cpp $(ROOT)/ps5/diag/diag_log.cpp $(LDFLAGS) $(shell $(PKG_CONFIG) --libs --static libarchive)
$(GEN)/extractor_elf.hpp: $(EXTRACTOR)
	mkdir -p $(GEN)
	{ echo 'const unsigned char extractor_elf[] = {'; xxd -i < $<; echo '};'; } > $@

# Linking SystemService before AppInstUtil is required: AppInstUtil alone hangs before main on the console.
# --wrap=pthread_create gives every thread a 512 KiB stack instead of the console's 64 KiB default.
$(ELF): $(PS5_SOURCES) $(PS5_HEADERS) $(APP_SOURCES) $(wildcard $(ROOT)/src/*.hpp) $(GEN)/ps5_assets.hpp $(GEN)/extractor_elf.hpp $(CA_BUNDLE) $(HTTP_DIR)/httplib.h $(JSON_DIR)/include/nlohmann/json.hpp icon0.png $(ROOT)/VERSION Makefile
	mkdir -p $(OUT)
	$(CXX) $(PS5_FLAGS) $(shell $(PKG_CONFIG) --cflags $(PACKAGES)) $(CXXFLAGS) -o $@ $(PS5_SOURCES) $(APP_SOURCES) $(LDFLAGS) -Wl,--wrap=pthread_create $(shell $(PKG_CONFIG) --libs --static $(PACKAGES)) -lSceSystemService -lSceAppInstUtil
	printf '%s\n' '$(ARCHIVEFETCH_VERSION)' > $(OUT)/ArchiveFetch.version

# The UI files come from web/, fonts and licenses from the desktop CMake build's web assets.
$(GEN)/ps5_assets.hpp: embed_assets.cmake icon0.png $(wildcard $(ROOT)/web/*) $(wildcard $(ASSET_DIR)/*)
	mkdir -p $(GEN)
	cmake -DROOT=$(abspath $(ROOT)) -DASSET_DIR=$(abspath $(ASSET_DIR)) -DOUTPUT=$(abspath $(GEN))/ps5_assets.hpp -P embed_assets.cmake
	touch $@

$(CA_BUNDLE):
	mkdir -p $(GEN)
	curl -fsSL --proto '=https' --tlsv1.2 -o $@.tmp https://curl.se/ca/cacert-$(CA_BUNDLE_DATE).pem
	echo "$(CA_BUNDLE_SHA256)  $@.tmp" | sha256sum -c -
	mv $@.tmp $@

check-deps:
	$(PKG_CONFIG) --exists $(PACKAGES)
	test -f $(HTTP_DIR)/httplib.h
	test -f $(JSON_DIR)/include/nlohmann/json.hpp