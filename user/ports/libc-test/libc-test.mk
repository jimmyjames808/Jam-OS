# libc-test (third_party/libc-test: musl's test suite) as one program,
# bin/libc-test in bootfs, included at the end of the Makefile after
# user/posix/posix.mk (docs/M13-PLAN.md, track P2). Each test of its
# functional and regression sets is compiled by tools/jamos-cc as upstream
# builds it (its CFLAGS, config.mak.def's, with the tests' tmpfile
# replaced: shim.h) with its main renamed (in the object), and every other global symbol
# of its object made local (objcopy), so that all of them link into one
# program with the runner (runner.c) and the table gen.py makes. The
# tests that need dynamic linking are not built (their lines in
# expected-failures.txt say skip). tools/libc-test.sh runs it.

LCT        := third_party/libc-test
LCTP       := user/ports/libc-test
LCT_OUT    := $(BUILD)/libc-test
LCT_LIST   := $(LCTP)/expected-failures.txt
LCT_TESTS  := $(shell python3 $(LCTP)/gen.py tests $(LCT) $(LCT_LIST))
LCT_OBJS   := $(LCT_TESTS:%=$(LCT_OUT)/test/%.o)
# Upstream's common code, less mtest (the math tests') and runtest (theirs).
LCT_COMMON := $(filter-out %/mtest.c %/runtest.c,$(wildcard $(LCT)/src/common/*.c))
LCT_COMMON_OBJS := $(patsubst $(LCT)/src/%.c,$(LCT_OUT)/%.o,$(LCT_COMMON))
LCT_DSO_OBJ := $(LCT_OUT)/functional/tls_align_dso.o

# config.mak.def's flags (the glibc-specific ones' -D_FILE_OFFSET_BITS=64
# is a no-op on musl), without -g, as the tests are linked stripped.
LCT_CFLAGS := -pipe -std=c99 -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Wall \
              -Wno-unused-function -Wno-missing-braces -Wno-unused -Wno-overflow \
              -Wno-unknown-pragmas -fno-builtin -frounding-math \
              -Werror=implicit-function-declaration -Werror=implicit-int \
              -Werror=pointer-sign -Werror=pointer-arith \
              -I$(LCT)/src/common -include $(LCTP)/shim.h
LCT_CC := JAMOS_SYSROOT=$(SYSROOT) CROSS=$(CROSS) tools/jamos-cc

# A test: main renamed (in the object, so that a main that falls off its
# end still returns 0) to what gen.py puts in the table, every other
# global of the object local.
$(LCT_OUT)/test/%.o: $(LCT)/src/%.c $(LCTP)/shim.h $(wildcard $(LCT)/src/common/*.h) \
                     $(SYSROOT_LIBS) tools/jamos-cc
	@mkdir -p $(dir $@)
	sym=lct_$$(echo $* | tr '/-' '__'); \
	$(LCT_CC) $(LCT_CFLAGS) -c $< -o $@.raw && \
	$(CROSS)objcopy --redefine-sym main=$$sym $@.raw $@.ren && \
	$(CROSS)objcopy --keep-global-symbol=$$sym $@.ren $@ && rm -f $@.raw $@.ren
$(LCT_OUT)/%.o: $(LCT)/src/%.c $(SYSROOT_LIBS) tools/jamos-cc
	@mkdir -p $(dir $@)
	$(LCT_CC) $(LCT_CFLAGS) -c $< -o $@
$(LCT_OUT)/runner.o: $(LCTP)/runner.c $(LCTP)/lct.h $(SYSROOT_LIBS) tools/jamos-cc
	@mkdir -p $(dir $@)
	$(LCT_CC) -std=gnu17 -O2 -Wall -Wextra -Werror -Wvla -D_GNU_SOURCE -c $< -o $@
$(LCT_OUT)/table.c: $(LCTP)/gen.py $(LCT_LIST) $(wildcard $(LCT)/src/functional/*.c \
                    $(LCT)/src/regression/*.c)
	@mkdir -p $(dir $@)
	python3 $(LCTP)/gen.py table $(LCT) $(LCT_LIST) > $@
$(LCT_OUT)/table.o: $(LCT_OUT)/table.c $(LCTP)/lct.h $(SYSROOT_LIBS) tools/jamos-cc
	$(LCT_CC) -std=gnu17 -O2 -Wall -Wextra -Werror -I$(LCTP) -c $< -o $@

LIBC_TEST := $(BUILD)/user/libc-test
$(LIBC_TEST): $(LCT_OBJS) $(LCT_COMMON_OBJS) $(LCT_DSO_OBJ) $(LCT_OUT)/runner.o \
              $(LCT_OUT)/table.o $(LCTP)/wants $(SYSROOT_LIBS) tools/jamos-cc
	@mkdir -p $(dir $@)
	$(LCT_CC) --jamos-wants=$(LCTP)/wants $(LCT_OBJS) $(LCT_COMMON_OBJS) $(LCT_DSO_OBJ) \
	    $(LCT_OUT)/runner.o $(LCT_OUT)/table.o -o $@
$(LIBC_TEST).bootfs: $(LIBC_TEST) tools/checkwants.py
	$(STRIP) --strip-debug $< -o $@
	python3 tools/checkwants.py $(LCTP)=$@
BOOTFS_FILES += bin/libc-test=$(LIBC_TEST).bootfs
$(BOOTFS): $(LIBC_TEST).bootfs
