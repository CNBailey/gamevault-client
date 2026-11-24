# GameVault for PS5 - payload build (needs the ps5-payload-sdk).
#   make                 build gamevault.elf (with the home-screen app inside)
#   make send PS5_HOST=192.168.0.50   send it to an ELF loader on port 9021
#   make -f Makefile.posix            Linux build for testing
#
# The home-screen app (app/) is built first with tools/build-app.sh, which
# needs clang-18, lld-18 and ninja; it fetches the UI kit and its SDKs itself.

PS5_HOST ?= ps5
PS5_PORT ?= 9021
VERSION  ?= $(shell cat VERSION 2>/dev/null || echo dev)

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined - see README.md)
endif

ELF     := gamevault.elf
COMMON  := src/util.c src/json.c src/settings.c src/http_server.c src/http_client.c \
           src/console.c src/downloads.c src/speedtest.c src/tile.c src/api.c src/main.c
ZLIB    := $(addprefix third_party/zlib/,adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c)
OBJS    := $(patsubst %.c,build/ps5/%.o,$(COMMON) src/platform_ps5.c build/assets.c) \
           $(patsubst %.c,build/ps5/%.o,$(ZLIB)) build/ps5/app_blob.o

CFLAGS  := -O2 -Wall -Werror -Wno-format-truncation -Isrc -Ithird_party/zlib -DGV_VERSION='"$(VERSION)"'
LDLIBS  := -lSceSystemService -lSceUserService -lSceAppInstUtil -lSceIpmi -lpthread

all: $(ELF)

build/assets.c: tools/embed.py $(shell find web/dist -type f 2>/dev/null)
	@test -f web/dist/index.html || (echo "Build the interface first: cd web && npm ci && npm run build" && false)
	python3 tools/embed.py web/dist $@

build/app/PPSA99688/eboot.bin: $(shell find app -type f) tools/build-app.sh VERSION
	tools/build-app.sh app

build/app.pack: build/app/PPSA99688/eboot.bin tools/pack-app.py
	python3 tools/pack-app.py build/app/PPSA99688 $@ build/app.id

build/ps5/app_blob.o: src/app_blob.S build/app.pack
	@mkdir -p $(@D)
	$(CC) -DGV_APP_PACK='"build/app.pack"' -c -o $@ $<

build/ps5/src/tile.o: src/tile.c build/app.pack $(wildcard src/*.h)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DGV_APP_ID='"$(shell cat build/app.id)"' -c -o $@ $<

build/ps5/third_party/zlib/%.o: third_party/zlib/%.c
	@mkdir -p $(@D)
	$(CC) -O2 -w -c -o $@ $<

build/ps5/%.o: %.c $(wildcard src/*.h)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(ELF): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)
	@ls -l $@ | awk '{printf "%s: %.1f MB\n", $$NF, $$5 / 1e6}'

send: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $(ELF)

clean:
	rm -rf $(ELF) build/ps5 build/assets.c build/app.pack build/app.id

.PHONY: all send clean
