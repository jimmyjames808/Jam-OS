# Lua 5.4 (MIT), docs/M13-PLAN.md question 9's "small C program":
# third_party/lua unmodified, the interpreter only (no luac), built with
# jamos-cc into the boot image as usr/bin/lua. lua.c and the libraries are
# upstream's, compiled as its Makefile does with LUA_USE_POSIX (popen,
# mkstemp, the POSIX time and file calls musl has) and without
# LUA_USE_LINUX/DLOPEN (no dynamic loading: `require` finds Lua files
# only, C modules are an error) and without readline (lua.c reads lines
# with fgets). No overrides: nothing in it forks. Its list is wants, a
# POSIX program that reads and writes files and runs commands (os.execute,
# io.popen: musl's system and popen, on /bin/sh).

LUA     := third_party/lua
LUA_DIR := $(PORTS)/lua
LUA_OUT := $(PORTS_OUT)/lua

LUA_SRCS := lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser \
            lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib \
            lmathlib loadlib loslib lstrlib ltablib lutf8lib linit lua
LUA_OBJS := $(LUA_SRCS:%=$(LUA_OUT)/obj/%.o)
LUA_CFLAGS := -std=gnu99 -O2 -w -DLUA_USE_POSIX -I$(LUA)/src -MMD -MP

$(LUA_OUT)/obj/%.o: $(LUA)/src/%.c | $(SYSROOT_HDRS) $(SYSROOT)/lib/libc.a
	@mkdir -p $(dir $@)
	$(PORT_CC) $(LUA_CFLAGS) -c $< -o $@

$(eval $(call PORT_PROGRAM,lua,lua,$(LUA_OBJS),$(LUA_DIR)/wants,,usr/bin/lua))
# The licence notice, in the image (MIT asks for it to travel with the program).
BOOTFS_FILES += share/licences/lua=$(LUA)/LICENSE
$(BOOTFS): $(LUA)/LICENSE
$(eval $(call PORT_LISTS,lua))

-include $(LUA_OBJS:.o=.d)
