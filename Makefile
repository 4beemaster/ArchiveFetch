ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to an installed ps5-payload-dev SDK with PacBrew ports)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

PS5_HOST ?= ps5
PS5_PORT ?= 9021
ELF := archive_catalog.elf
SOURCES := src/main.cpp src/application.cpp src/catalog.cpp src/transfer.cpp
HEADERS := src/application.hpp src/catalog.hpp src/transfer.hpp
PACKAGES := libcurl libxml-2.0 openssl
CPPFLAGS += -DPS5_PAYLOAD=1 -Isrc $(shell $(PKG_CONFIG) --cflags $(PACKAGES))
CXXFLAGS += -std=c++17 -O2 -Wall -Wextra
LDLIBS += $(shell $(PKG_CONFIG) --libs --static $(PACKAGES))

.PHONY: all check-deps deploy clean
all: $(ELF)

check-deps:
	$(PKG_CONFIG) --exists $(PACKAGES)

$(ELF): $(SOURCES) $(HEADERS) | check-deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

clean:
	rm -f $(ELF)