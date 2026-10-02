/* The boot words read strictly: cmdline_vlan, the network's mode (a VLAN,
 * untagged or off), which fails closed (anything that isn't a VLAN id or
 * an untagged word is the network off). */
#include <stdint.h>
#include <jam/cmdline.h>
#include <jam/ktest.h>

KTEST(cmdline_vlan_words)
{
    /* No word: the default (the build's, JAMOS_NET_DEFAULT). */
    KT_EQ(cmdline_vlan("", 21), 21);
    KT_EQ(cmdline_vlan("shell verbose", 21), 21);
    KT_EQ(cmdline_vlan("shell", 0), 0);
    /* A VLAN id, 1..4094, in decimal. */
    KT_EQ(cmdline_vlan("vlan=21", 7), 21);
    KT_EQ(cmdline_vlan("shell vlan=1 verbose", 21), 1);
    KT_EQ(cmdline_vlan("vlan=4094", 21), 4094);
    KT_EQ(cmdline_vlan("vlan=0021", 7), 21);
    /* Anything else is no VLAN, whatever the default. */
    KT_EQ(cmdline_vlan("vlan=off", 21), 0);
    KT_EQ(cmdline_vlan("vlan=0", 21), 0);
    KT_EQ(cmdline_vlan("vlan=4095", 21), 0);
    KT_EQ(cmdline_vlan("vlan=65557", 21), 0);       /* 21 + 65536: no truncation */
    KT_EQ(cmdline_vlan("vlan=21x", 21), 0);
    KT_EQ(cmdline_vlan("vlan=-21", 21), 0);
    KT_EQ(cmdline_vlan("vlan= 21", 21), 0);         /* "vlan=" alone, then a stray word */
    KT_EQ(cmdline_vlan("vlan", 21), 0);
    KT_EQ(cmdline_vlan("verbose vlan=", 21), 0);
    KT_EQ(cmdline_vlan("vlan=00021", 21), 0);       /* more than 4 digits */
    /* Two words: the same id is that id; two different ones are none. */
    KT_EQ(cmdline_vlan("vlan=21 vlan=21", 7), 21);
    KT_EQ(cmdline_vlan("vlan=21 vlan=22", 7), 0);
    KT_EQ(cmdline_vlan("vlan=21 vlan=off", 7), 0);
    KT_EQ(cmdline_vlan("vlan=off vlan=21", 7), 0);
    /* Words that only start like it are other words. */
    KT_EQ(cmdline_vlan("vlanx=3 novlan avlan=5", 21), 21);
}

/* The untagged mode: vlan=none or vlan=untagged, exactly; as a default it
 * is what a boot with no word gets. */
KTEST(cmdline_vlan_untagged)
{
    const uint32_t U = CMDLINE_VLAN_UNTAGGED;
    KT_EQ(cmdline_vlan("vlan=none", 21), U);
    KT_EQ(cmdline_vlan("shell vlan=untagged verbose", 21), U);
    KT_EQ(cmdline_vlan("", U), U);
    KT_EQ(cmdline_vlan("shell nosplash", U), U);
    KT_EQ(cmdline_vlan("vlan=21", U), 21);
    KT_EQ(cmdline_vlan("vlan=off", U), 0);
    /* Not quite the words: the network off, whatever the default. */
    KT_EQ(cmdline_vlan("vlan=None", 21), 0);
    KT_EQ(cmdline_vlan("vlan=nonex", U), 0);
    KT_EQ(cmdline_vlan("vlan=non", U), 0);
    KT_EQ(cmdline_vlan("vlan=untag", 21), 0);
    KT_EQ(cmdline_vlan("vlan=4096", U), 0);          /* U's number is no VLAN id */
    KT_EQ(cmdline_vlan("vlan=untaggedx", 21), 0);
    /* Two words: both untagged agree; untagged and a VLAN don't. */
    KT_EQ(cmdline_vlan("vlan=none vlan=untagged", 21), U);
    KT_EQ(cmdline_vlan("vlan=none vlan=21", 21), 0);
    KT_EQ(cmdline_vlan("vlan=21 vlan=none", U), 0);
    KT_EQ(cmdline_vlan("vlan=none vlan=off", 21), 0);
    /* U is no VLAN id: 13 bits. */
    KT_ASSERT(U > 4095);
}
