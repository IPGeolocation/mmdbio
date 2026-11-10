# mmdbio: a static binary with no dependencies beyond libc (zlib is bundled).
#   make            builds build/mmdbio
#   make test       builds and runs the tests
#   make install    copies it to $(PREFIX)/bin (default /usr/local), and the
#                   library and its header to $(PREFIX)/lib and include
#   make lib        build/libmmdbio.a, the library (include/mmdbio.h)
# Other targets build with another compiler, e.g.
#   make CC=x86_64-w64-mingw32-gcc BUILD=build-windows    (Windows, .exe)
#   make CC=i686-linux-gnu-gcc BUILD=build-386            (32-bit x86, GCC 14+;
#                                                          or CC="gcc -m32" with gcc-multilib)
# This is a GNU make file: on the BSDs and illumos, run gmake.
CC      ?= cc
CFLAGS  ?= -O2
# kept when CFLAGS is given on the command line (make CFLAGS=-O3)
override CFLAGS += -std=c11 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Werror -Ithird_party/zlib
ZFLAGS  := -O2 -std=c11 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -D_FILE_OFFSET_BITS=64 -w -Ithird_party/zlib
PREFIX  ?= /usr/local
UNAME_S := $(shell uname -s)
MACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)
EXE :=
ifneq (,$(findstring mingw,$(MACHINE))$(findstring windows,$(MACHINE)))
  EXE := .exe
  # GCC checks formats against the Microsoft runtime here; the C99 stdio
  # below takes them all, and the Linux build checks them
  override CFLAGS += -D__USE_MINGW_ANSI_STDIO=1 -Wno-format
  ZFLAGS += -D__USE_MINGW_ANSI_STDIO=1
  LDFLAGS ?= -static
  LDLIBS += -lpsapi
else ifeq ($(UNAME_S),Darwin)
  LDFLAGS ?=
else ifeq ($(UNAME_S),SunOS)
  # Solaris and illumos have no static libc; -std=c11 hides POSIX without this
  override CFLAGS += -D__EXTENSIONS__
  ZFLAGS += -D__EXTENSIONS__
  LDFLAGS ?=
  LDLIBS += -lsocket -lnsl
else
  LDFLAGS ?= -static
endif
LDLIBS  += -lm

BUILD := build
ZLIB_SRC := adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c
ZLIB_OBJ := $(addprefix $(BUILD)/zlib/,$(ZLIB_SRC:.c=.o))
CORE := common utf8 csv sha256 hashtab mmdb_enc mmdb_read json streamwriter netaddr flags jsonw input jstream \
	rows tables tmpl shared bconfig engine compat color tabular shrink shrink_check cmd_common cmd_read cmd_info cmd_export cmd_diff \
	cmd_completion cmd_import cmd_shrink
CORE_OBJ := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE)))
HEADERS := $(wildcard src/*.h) $(wildcard third_party/zlib/*.h) include/mmdbio.h
# the binutils that go with $(CC): x86_64-w64-mingw32-gcc -> x86_64-w64-mingw32-ar
TOOLPREFIX := $(patsubst %gcc,%,$(filter %-gcc,$(firstword $(CC))))
# (clang targets: pass the matching ones, e.g. LIBAR=llvm-ar NM=llvm-nm OBJCOPY=llvm-objcopy,
# or i686-w64-mingw32-ar/nm/objcopy for 32-bit Windows, whose objects llvm-objcopy can't edit)
LIBAR ?= $(TOOLPREFIX)ar
OBJCOPY ?= $(TOOLPREFIX)objcopy
NM ?= $(TOOLPREFIX)nm

all: $(BUILD)/mmdbio$(EXE)

$(BUILD)/zlib/%.o: third_party/zlib/%.c $(wildcard third_party/zlib/*.h)
	@mkdir -p $(BUILD)/zlib
	$(CC) $(ZFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/mmdbio$(EXE): $(CORE_OBJ) $(BUILD)/main.o $(ZLIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# The library: every object linked into one, with only the mmdbio_ names
# left global (where objcopy is there), so its internals can't collide with
# the program it goes into.
lib: $(BUILD)/libmmdbio.a

$(BUILD)/libmmdbio.a: $(CORE_OBJ) $(BUILD)/libmmdbio.o $(ZLIB_OBJ)
	$(CC) $(CFLAGS) -r -nostdlib -o $(BUILD)/libmmdbio-all.o $^
	$(NM) -g --defined-only $(BUILD)/libmmdbio.o | awk '{print $$3}' > $(BUILD)/libmmdbio.exports
	# 32-bit x86 PIC thunks are shared COMDAT copies: hidden here, they would
	# replace libc's own and leave its references unresolved
	$(NM) -g --defined-only $(BUILD)/libmmdbio-all.o | awk '$$3 ~ /^__x86\.get_pc_thunk/ {print $$3}' >> $(BUILD)/libmmdbio.exports
	@# an objcopy that can't (llvm-objcopy on Windows objects) leaves every name global.
	@# clang's .llvm_addrsig loses its discard flag in the -r link on Windows; kept,
	@# it is placed at address 0 and the program won't load, so it goes either way
	if ! { command -v $(OBJCOPY) > /dev/null 2>&1 && \
	       $(OBJCOPY) -R .llvm_addrsig --keep-global-symbols=$(BUILD)/libmmdbio.exports $(BUILD)/libmmdbio-all.o $(BUILD)/libmmdbio-one.o; }; then \
		echo "note: $(OBJCOPY) can't hide the library's internal names; they stay global"; \
		$(OBJCOPY) -R .llvm_addrsig $(BUILD)/libmmdbio-all.o $(BUILD)/libmmdbio-one.o 2> /dev/null || \
			cp $(BUILD)/libmmdbio-all.o $(BUILD)/libmmdbio-one.o; \
	fi
	rm -f $@
	$(LIBAR) rcs $@ $(BUILD)/libmmdbio-one.o

$(BUILD)/lib-test$(EXE): tests/lib_test.c include/mmdbio.h $(BUILD)/libmmdbio.a
	$(CC) $(CFLAGS) -Iinclude -o $@ tests/lib_test.c $(BUILD)/libmmdbio.a $(LDFLAGS) $(LDLIBS)

$(BUILD)/unit-tests$(EXE): $(CORE_OBJ) $(BUILD)/unit_tests.o $(ZLIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD)/unit_tests.o: tests/unit_tests.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

PYTHON ?= python3

test: all $(BUILD)/unit-tests$(EXE) $(BUILD)/lib-test$(EXE)
	$(BUILD)/unit-tests$(EXE)
	@mkdir -p $(BUILD)/lib-test.d
	$(BUILD)/lib-test$(EXE) $(BUILD)/lib-test.d
	PYTHON=$(PYTHON) sh tests/run_tests.sh $(BUILD)/mmdbio$(EXE)

install: all lib
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 644 $(BUILD)/libmmdbio.a $(DESTDIR)$(PREFIX)/lib/libmmdbio.a
	install -m 644 include/mmdbio.h $(DESTDIR)$(PREFIX)/include/mmdbio.h
	install -m 755 $(BUILD)/mmdbio$(EXE) $(DESTDIR)$(PREFIX)/bin/mmdbio$(EXE)

clean:
	rm -rf $(BUILD)

.PHONY: all lib test install clean
