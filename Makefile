SYSROOT ?= $(HOME)/.cache/q2/sysroot
Q2_CC ?= clang
Q2_LD ?= ld.lld
CFLAGS ?= -O2 -Wall -Wextra -Werror
Q2_FLAGS = --target=mipsel-linux-gnu --sysroot="$(SYSROOT)" -march=mips32r2 -mabi=32 -mfp64 -mnan=2008 -mabs=2008
Q2_OBJECTS = build/q2/app.o build/q2/platform_q2.o build/q2/crt-q2.o build/q2/q2pod_scheme.o build/q2/worldclock.o build/q2/extra_tools.o
CARD = dist/.utilities-app

all: dist/.utilities-app.zip

check-sysroot:
	@test -r "$(SYSROOT)/usr/include/time.h" && \
		test -r "$(SYSROOT)/usr/include/mipsel-linux-gnu/gnu/stubs-o32_hard_2008.h" && \
		test -r "$(SYSROOT)/lib/libc.so.6" && test -r "$(SYSROOT)/lib/libgcc_s.so.1" && \
		test -r "$(SYSROOT)/lib/ld-linux-mipsn8.so.1" || { \
		echo 'Q2 sysroot is incomplete at $(SYSROOT).' >&2; \
		echo 'Run: bash tools/mksysroot.sh "Q2 Firmware V1.36.zip" "$(SYSROOT)"' >&2; exit 1; }

build/q2/%.o: src/%.c src/app.h src/font.h src/q2pod_scheme.h src/worldclock.h src/extra_tools.h src/utility_icons.h | check-sysroot
	mkdir -p $(@D)
	$(Q2_CC) $(Q2_FLAGS) $(CFLAGS) -std=gnu11 -c $< -o $@

$(CARD)/q2-utilities-app: $(Q2_OBJECTS)
	mkdir -p $(@D)
	$(Q2_LD) -m elf32ltsmip -e __start --dynamic-linker /lib/ld-linux-mipsn8.so.1 \
		--image-base=0x400000 -z noexecstack --hash-style=sysv -s -o $@ $^ \
		"$(SYSROOT)/lib/libc.so.6" "$(SYSROOT)/lib/libgcc_s.so.1" "$(SYSROOT)/lib/ld-linux-mipsn8.so.1"

$(CARD)/q2app: q2app
	mkdir -p $(@D)
	cp $< $@

dist/.utilities-app.zip: $(CARD)/q2-utilities-app $(CARD)/q2app
	cd dist && rm -f .utilities-app.zip && zip -q .utilities-app.zip .utilities-app/q2app .utilities-app/q2-utilities-app

build/host/check: tests/check.c src/app.c src/q2pod_scheme.c src/worldclock.c src/extra_tools.c src/app.h src/q2pod_scheme.h src/worldclock.h src/extra_tools.h src/utility_icons.h src/font.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Isrc -o $@ tests/check.c src/app.c src/q2pod_scheme.c src/worldclock.c src/extra_tools.c

build/host/test_scheme: tests/test_scheme.c src/q2pod_scheme.c src/q2pod_scheme.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Isrc -o $@ tests/test_scheme.c src/q2pod_scheme.c

build/host/test_worldclock: tests/test_worldclock.c src/worldclock.c src/worldclock.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Isrc -o $@ tests/test_worldclock.c src/worldclock.c

build/host/test_extra_tools: tests/test_extra_tools.c src/extra_tools.c src/extra_tools.h
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Isrc -o $@ tests/test_extra_tools.c src/extra_tools.c

check: build/host/check build/host/test_scheme build/host/test_worldclock build/host/test_extra_tools
	./build/host/check
	./build/host/test_scheme
	./build/host/test_worldclock
	./build/host/test_extra_tools

clean:
	rm -rf build dist

.PHONY: all check clean check-sysroot
