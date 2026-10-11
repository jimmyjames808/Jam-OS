# tinyssh, the SSH server (docs/M13-PLAN.md section 15 and "X2 notes"),
# included at the end of the Makefile after user/posix/posix.mk: tinyssh
# 20261001 (third_party/tinyssh, vendored unmodified) built with
# tools/jamos-cc on the sysroot, by these rules instead of its Makefile and
# configure probes, as one program, bin/sshd: the listener (sshd.c), one
# tinysshd per connection and its two helpers, each a mode by argv[0]
# (tinysshd.c).
#
#   user/ports/tinyssh/tinyssh-features.h   the probes' answers, fixed: every
#                                   has*.h tinyssh includes is made a line
#                                   that includes it
#   user/ports/tinyssh/OVERRIDES    the upstream files replaced by ours
#                                   (each says why), with upstream's hashes,
#                                   checked before the build and by
#                                   `make check` (tools/checkoverrides.py)
#   user/ports/tinyssh/wants        bin/sshd's list (<wants.h>)
#
# Left out of upstream's sources, and why:
#   channel_fork.c, channel_forkpty.c, channel_drop.c, loginshell.c,
#   dropuidgid.c   the session's child is one of init's sessions
#                  (channel.c's override): no fork, pty, setuid or
#                  execve of a login shell
#   logsys.c       login records (utmp): none on Jam OS
#   main_tinysshd_makekey.c, main_tinysshd_printkey.c   init makes the
#                  host key and the shell's `ssh` shows its fingerprint
#   crypto_*_lib25519.c, *_libntruprime.c, *_lib1305.c   the outside
#                  libraries' variants (none ported): tinyssh's own crypto
#   _tinysshd-speed.c, has*.c   a benchmark and the probes

TS_SRC  := third_party/tinyssh
TS_PORT := user/ports/tinyssh
TS_OUT  := $(BUILD)/tinyssh
TS_OVR  := $(TS_PORT)/OVERRIDES
TS_BIN  := $(BUILD)/user/sshd

TS_OVR_UP   := $(shell awk '!/^\#/ && NF == 3 { print $$1 }' $(TS_OVR))
TS_OVR_OURS := $(shell awk '!/^\#/ && NF == 3 { print $$3 }' $(TS_OVR))
TS_LEFT_OUT := channel_fork.c channel_forkpty.c channel_drop.c loginshell.c dropuidgid.c \
               logsys.c main_tinysshd_makekey.c main_tinysshd_printkey.c _tinysshd-speed.c \
               crypto_hash_sha512_lib25519.c crypto_kem_sntrup761_libntruprime.c \
               crypto_onetimeauth_poly1305_lib1305.c crypto_sign_ed25519_lib25519.c
TS_UP_SRCS  := $(filter-out $(addprefix $(TS_SRC)/,has%.c $(TS_LEFT_OUT) $(TS_OVR_UP)), \
                            $(wildcard $(TS_SRC)/*.c))
TS_OURS     := $(wildcard $(TS_PORT)/*.c)
TS_OBJS     := $(TS_UP_SRCS:$(TS_SRC)/%.c=$(TS_OUT)/up/%.o) $(TS_OURS:$(TS_PORT)/%.c=$(TS_OUT)/ours/%.o)

# The headers tinyssh's Makefile makes with its probes (its AUTOHEADERS).
TS_FEATURES := haslib1305 haslib25519 haslibntruprime haslibrandombytes haslibutilh haslimits \
               haslogintty hasmlock hasopenpty hasutilh hasutmp hasutmpaddrv6 hasutmphost \
               hasutmploginlogout hasutmplogwtmp hasutmpname hasutmppid hasutmptime hasutmptv \
               hasutmptype hasutmpuser hasutmpx hasutmpxaddrv6 hasutmpxsyslen hasutmpxupdwtmpx \
               hasvalgrind
TS_HDRS     := $(TS_FEATURES:%=$(TS_OUT)/inc/%.h)
TS_CHECKED  := $(TS_OUT)/overrides.ok

TS_CC     := JAMOS_SYSROOT=$(SYSROOT) CROSS=$(CROSS) tools/jamos-cc
# tinyssh's own flags (-Os -fwrapv, its warnings), on the sysroot; ours
# add -Werror. Its headers (and ours) by -iquote: a name of theirs never
# hides a system header.
TS_CFLAGS := -std=gnu99 -Os -fwrapv -W -Wall -MMD -MP -iquote $(TS_OUT)/inc -iquote $(TS_PORT) \
             -iquote $(TS_SRC) -iquote $(TS_SRC)/cryptoint

$(TS_CHECKED): $(TS_OVR) tools/checkoverrides.py $(addprefix $(TS_SRC)/,$(TS_OVR_UP)) \
               $(TS_OVR_OURS)
	@mkdir -p $(dir $@)
	python3 tools/checkoverrides.py $(TS_OVR) $(TS_SRC)
	touch $@

$(TS_HDRS): $(TS_OUT)/inc/%.h: $(TS_PORT)/tinyssh-features.h
	@mkdir -p $(dir $@)
	printf '#include "tinyssh-features.h"\n' > $@

# (jamos-cc wants the whole sysroot, libc.a too, even to compile.)
$(TS_OUT)/up/%.o: $(TS_SRC)/%.c $(TS_PORT)/tinyssh-features.h | $(TS_HDRS) $(TS_CHECKED) $(SYSROOT_LIBS) \
                  $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(TS_CC) $(TS_CFLAGS) -c $< -o $@
$(TS_OUT)/ours/%.o: $(TS_PORT)/%.c $(TS_PORT)/tinyssh-features.h | $(TS_HDRS) $(TS_CHECKED) \
                    $(SYSROOT_LIBS) $(SYSROOT_HDRS)
	@mkdir -p $(dir $@)
	$(TS_CC) $(TS_CFLAGS) -Werror -c $< -o $@

$(TS_BIN): $(TS_OBJS) $(TS_PORT)/wants $(SYSROOT_LIBS) tools/jamos-cc
	@mkdir -p $(dir $@)
	$(TS_CC) --jamos-wants=$(TS_PORT)/wants $(TS_OBJS) -o $@
$(TS_BIN).bootfs: $(TS_BIN) tools/checkwants.py
	$(STRIP) --strip-debug $< -o $@
	python3 tools/checkwants.py $(TS_PORT)=$@
BOOTFS_FILES += bin/sshd=$(TS_BIN).bootfs
$(BOOTFS): $(TS_BIN).bootfs

# `make check`: the overrides still match upstream.
.PHONY: tinyssh-check
tinyssh-check:
	python3 tools/checkoverrides.py $(TS_OVR) $(TS_SRC)
check: tinyssh-check

-include $(TS_OBJS:.o=.d)
