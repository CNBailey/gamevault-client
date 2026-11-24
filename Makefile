# GameVault for PS5 - payload build (needs the ps5-payload-sdk).
#   make                 build gamevault.elf
#   make send PS5_HOST=192.168.0.50   send it to an ELF loader on port 9021
#   make -f Makefile.posix            Linux build for testing

PS5_HOST ?= ps5
PS5_PORT ?= 9021
VERSION  ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined - see README.md)
endif

ELF     := gamevault.elf
COMMON  := src/util.c src/json.c src/settings.c src/http_server.c src/http_client.c \
           src/console.c src/downloads.c src/api.c src/main.c
SRCS    := $(COMMON) src/platform_ps5.c build/assets.c

CFLAGS  := -O2 -Wall -Werror -Wno-format-truncation -Isrc -DGV_VERSION='"$(VERSION)"'
LDLIBS  := -lSceSystemService -lSceUserService -lSceAppInstUtil -lSceIpmi -lpthread

all: $(ELF)

build/assets.c: tools/embed.py $(shell find web/dist -type f 2>/dev/null)
	@test -f web/dist/index.html || (echo "Build the interface first: cd web && npm ci && npm run build" && false)
	python3 tools/embed.py web/dist $@

$(ELF): $(SRCS) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

send: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

clean:
	rm -rf $(ELF) build

.PHONY: all send clean
