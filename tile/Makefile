# GameVault home-screen tile: a fake-signed eboot.bin plus its sce_sys folder.
# Built by the top-level Makefile; needs the ps5-payload-sdk.

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

# From the SDK's install_app sample.
PS4_SDK_VERSION ?= 0x07590001
AUTHID   := 0x3800000000000022
AUTHINFO := 00 00 00 00 00 00 00 00 00 00 00 00 00 1C 00 40 00 FF 00 00 00 00 \
            00 80 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 80 \
            00 40 00 40 00 00 00 00 00 00 00 80 00 00 00 00 00 00 00 08 00 40 \
            FF FF 00 00 00 F0 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
            00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
            00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
            00 00 00 00

OUT := ../build/tile

all: $(OUT)/eboot.bin

$(OUT)/eboot.o: eboot.c
	@mkdir -p $(OUT)
	$(CC) -c -O2 -Wall -Werror -ffreestanding -fno-stack-protector -fno-builtin -o $@ $<

$(OUT)/eboot.elf: $(OUT)/eboot.o eboot.x
	$(LD) --static -T eboot.x -o $@ $<

$(OUT)/eboot.bin: $(OUT)/eboot.elf make_fself.py
	python3 make_fself.py --ptype fake --paid $(AUTHID) \
	    --auth-info "$(AUTHINFO)" \
	    --app-version $(PS4_SDK_VERSION) \
	    --fw-version $(PS4_SDK_VERSION) $< $@

.PHONY: all
