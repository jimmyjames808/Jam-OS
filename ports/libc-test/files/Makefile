# The rules of the libc-test program (ports/libc-test/build.sh runs this
# with GNU make in the unpacked source). Each test of libc-test's
# functional and regression sets is compiled as upstream builds it
# (config.mak.def's flags, with the tests' tmpfile replaced: shim.h) with
# its main renamed, and every other global symbol of its object made local
# (objcopy), so that all of them link into one program with the runner
# (runner.c) and the table gen.py makes. The tests that need dynamic
# linking are not built (their lines in expected-failures.txt say skip).
#
#   SRC        the source directory
#   PORTFILES  this directory
#   TESTS      "set/name" for each test to build (gen.py tests)
#   CC, OBJCOPY set by the builder

# config.mak.def's flags (the glibc-specific ones' -D_FILE_OFFSET_BITS=64
# is a no-op on musl), without -g, as the tests are linked stripped. The
# last option makes the tests' __FILE__ (their error messages) name the
# file in the repository, third_party/libc-test/src/...
CFLAGS := -pipe -std=c99 -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Wall \
          -Wno-unused-function -Wno-missing-braces -Wno-unused -Wno-overflow \
          -Wno-unknown-pragmas -fno-builtin -frounding-math \
          -Werror=implicit-function-declaration -Werror=implicit-int \
          -Werror=pointer-sign -Werror=pointer-arith \
          -I$(SRC)/src/common -include $(PORTFILES)/shim.h \
          -ffile-prefix-map=$(SRC)=third_party/libc-test

TEST_OBJS   := $(TESTS:%=test/%.o)
# Upstream's common code, less mtest (the math tests') and runtest (theirs).
COMMON      := $(filter-out %/mtest.c %/runtest.c,$(sort $(wildcard $(SRC)/src/common/*.c)))
COMMON_OBJS := $(patsubst $(SRC)/src/%.c,%.o,$(COMMON))
DSO_OBJ     := functional/tls_align_dso.o

libc-test: $(TEST_OBJS) $(COMMON_OBJS) $(DSO_OBJ) runner.o table.o
	$(CC) -o $@ $^

# A test: main renamed (in the object, so that a main that falls off its
# end still returns 0) to what gen.py puts in the table, every other
# global of the object local.
test/%.o: $(SRC)/src/%.c $(PORTFILES)/shim.h
	@mkdir -p $(dir $@)
	sym=lct_$$(echo $* | tr '/-' '__'); \
	$(CC) $(CFLAGS) -c $< -o $@.raw && \
	$(OBJCOPY) --redefine-sym main=$$sym $@.raw $@.ren && \
	$(OBJCOPY) --keep-global-symbol=$$sym $@.ren $@ && rm -f $@.raw $@.ren
# Upstream's common code and the one helper object, as they are (the
# directory in the pattern: without a slash make would put it in front of
# the prerequisite).
common/%.o: $(SRC)/src/common/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
functional/%.o: $(SRC)/src/functional/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
runner.o: $(PORTFILES)/runner.c $(PORTFILES)/lct.h
	$(CC) -std=gnu17 -O2 -Wall -Wextra -Werror -Wvla -D_GNU_SOURCE -c $< -o $@
table.c: $(PORTFILES)/gen.py $(PORTFILES)/expected-failures.txt
	python3 $(PORTFILES)/gen.py table $(SRC) $(PORTFILES)/expected-failures.txt > $@
table.o: table.c $(PORTFILES)/lct.h
	$(CC) -std=gnu17 -O2 -Wall -Wextra -Werror -I$(PORTFILES) -c $< -o $@
