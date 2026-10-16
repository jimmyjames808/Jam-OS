/* init: asking volumes to make an NVMe drive a Jam OS disk
 * (initctl.prepare_disk, abi/idl/initctl.idl; M12.7 stage 2), or to format
 * a Jam OS disk's data partition again (initctl.format_data; M13.6's U7b:
 * ext, or FAT32 when asked; the serial's end is the confirmation). The shell has
 * shown the owner the drive's model, serial number and size (and, for a
 * drive that isn't empty, what is on it) and had the last four characters
 * of the serial typed (and `erase`) before it calls; init passes the disk's
 * id and that confirmation on to volumes on the install channel
 * (volinstall.prepare, abi/idl/volumes.idl), the only channel that can ask
 * it, which init made when it started volumes (VOLUMES_SR_INSTALL) and
 * gives to nobody. volumes checks the drive and the confirmation again with
 * code of its own and the driver a third time (drivers/nvme refuses
 * everything but a blank drive, or one whose erase the owner confirmed), so
 * nothing here decides what may be written.
 *
 * The call takes a while (the table, then both partitions formatted and
 * mounted: seconds on QEMU's drive, longer on a big one), so ctl.c sends it
 * without waiting and answers the shell when volumes' answer comes to its
 * loop, as it does for mount and sync (its `laters`); this file has what is
 * particular to the call: the channel, the request, the answer and what is
 * said in the log. */
#include <idl/volumes.h>
#include "init.h"

status_t prepare_send(uint32_t txid, uint32_t id, const char *tail, uint32_t tail_len,
                      const char *word, uint32_t word_len)
{
    handle_t vm = shell_volumes_install();
    return vm ? volinstall_prepare_send(vm, txid, id, tail, tail_len, word, word_len)
              : ERR_NOT_FOUND;
}

status_t prepare_result(const void *rep, struct idl_msg *m)
{
    return volinstall_prepare_result(rep, m);
}

void prepare_say(uint32_t id, status_t st, uint64_t t0)
{
    printf("init: prepare disk %08x: %s (%lu ms)\n", id, status_str(st),
           (unsigned long)((now() - t0) / NS_PER_MS));
}

/* initctl.format_data (the shell's `disk format nvme1 data [fat]`): a Jam
 * OS disk's data partition formatted again, the same way. */
status_t format_send(uint32_t txid, uint32_t id, const char *tail, uint32_t tail_len, bool fat)
{
    handle_t vm = shell_volumes_install();
    return vm ? volinstall_format_data_send(vm, txid, id, tail, tail_len, fat ? 1 : 0)
              : ERR_NOT_FOUND;
}

status_t format_result(const void *rep, struct idl_msg *m)
{
    return volinstall_format_data_result(rep, m);
}

void format_say(uint32_t id, status_t st, uint64_t t0)
{
    printf("init: format disk %08x's data partition: %s (%lu ms)\n", id, status_str(st),
           (unsigned long)((now() - t0) / NS_PER_MS));
}
