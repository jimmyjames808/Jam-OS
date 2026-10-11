# The rules of sbase's three programs (ports/sbase/build.sh runs this with
# GNU make in the unpacked source): sbase's own Makefile builds libutf.a,
# libutil.a and a program a tool; here each tool's object has its main
# renamed <tool>_main and three generated mains (mkbox.sh) pick one by
# the name the program was started as.
#
#   SRC    the source directory (overrides already in it)
#   PORTFILES  this directory
#   BOXES  the programs' names: sbase-plain sbase-ro sbase-rw
#   TOOLS_sbase-plain ...  each program's tools
#   CC, AR set by the builder

# The last option makes __FILE__ (dc.c's assert) name the file in the
# repository, third_party/sbase/...
CFLAGS := -std=c99 -O2 -w -D_DEFAULT_SOURCE -D_NETBSD_SOURCE -D_BSD_SOURCE \
          -D_XOPEN_SOURCE=700 -D_FILE_OFFSET_BITS=64 -I$(SRC) \
          -ffile-prefix-map=$(SRC)=third_party/sbase

UTF  := $(patsubst $(SRC)/libutf/%.c,libutf/%.o,$(sort $(wildcard $(SRC)/libutf/*.c)))
UTIL := $(patsubst $(SRC)/libutil/%.c,libutil/%.o,$(sort $(wildcard $(SRC)/libutil/*.c)))
LIBS := libutil.a libutf.a

all: $(BOXES)

libutf/%.o: $(SRC)/libutf/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
libutil/%.o: $(SRC)/libutil/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
libutf.a: $(UTF)
	rm -f $@
	$(AR) rcs $@ $^
libutil.a: $(UTIL)
	rm -f $@
	$(AR) rcs $@ $^

tool/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Dmain=$(subst -,_,$*)_main -c $< -o $@

# $(call BOX,<program>): its main, then the link (the output's name picks
# the program's list: the builder's compiler).
define BOX
box-$(1).c: $(PORTFILES)/mkbox.sh
	sh $(PORTFILES)/mkbox.sh $(1) $$(TOOLS_$(1)) > $$@
box-$(1).o: box-$(1).c
	$(CC) $(CFLAGS) -c $$< -o $$@
$(1): box-$(1).o $$(TOOLS_$(1):%=tool/%.o) $(LIBS)
	$(CC) -o $$@ $$^
endef
$(foreach b,$(BOXES),$(eval $(call BOX,$(b))))
