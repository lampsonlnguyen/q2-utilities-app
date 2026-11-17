SYSROOT ?= $(HOME)/.cache/q2/sysroot
Q2_CC ?= clang
Q2_LD ?= ld.lld
CFLAGS ?= -O2 -Wall -Wextra -Werror
Q2_FLAGS = --target=mipsel-linux-gnu --sysroot="$(SYSROOT)" -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008
Q2_OBJECTS = build/q2/app.o build/q2/platform_q2.o build/q2/crt-q2.o
CARD = dist/.utilities

all: dist/utilities.zip

build/q2/%.o: src/%.c src/app.h src/font.h
	mkdir -p $(@D)
	$(Q2_CC) $(Q2_FLAGS) $(CFLAGS) -std=gnu11 -c $< -o $@

$(CARD)/q2-utilities: $(Q2_OBJECTS)
	mkdir -p $(@D)
	$(Q2_LD) -m elf32ltsmip -e __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 \
		--image-base=0x400000 -z noexecstack --hash-style=sysv -s -o $@ $^ \
		"$(SYSROOT)/lib/libc.so.6" "$(SYSROOT)/lib/libgcc_s.so.1" "$(SYSROOT)/lib/ld-linux-mipsn8.so.1"

$(CARD)/q2app: q2app
	mkdir -p $(@D)
	cp $< $@

dist/utilities.zip: $(CARD)/q2-utilities $(CARD)/q2app
	cd dist && zip -q utilities.zip .utilities/q2app .utilities/q2-utilities

build/host/check: tests/check.c src/app.c src/app.h src/font.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Isrc -o $@ tests/check.c src/app.c

check: build/host/check
	./build/host/check

clean:
	rm -rf build dist

.PHONY: all check clean
