/* usb (lsusb): the USB devices usb-bus found, with their interfaces and
 * the hubs they are behind. */
#include <devmgr.h>
#include <idl/usbbus.h>
#include "../sh.h"

#define MAX_SEEN 64

struct seen {
    uint32_t id;
    uint16_t vendor, product;
};

/* usb-bus: the bound driver that answers usbbus.status (as usbtest finds it). */
static handle_t find_usb_bus(void)
{
    handle_t dm;
    for (uint32_t n = 0; (dm = sh_devmgr()) && n < 16; n++) {
        struct devmgr_rep r;
        handle_t hs[1];
        uint32_t nh = 0;
        status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, hs, 1, &nh,
                                  (uint64_t)jam_clock_get() + 5 * SH_S);
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK || nh != 1)
            continue;
        uint32_t d, h, i, hid, p, g;
        uint8_t settled;
        if (usbbus_status_until(hs[0], (uint64_t)jam_clock_get() + 2 * SH_S, &d, &h, &i, &hid, &p,
                                &g, &settled) == OK)
            return hs[0];
        jam_handle_close(hs[0]);
    }
    return HANDLE_INVALID;
}

static const char *usb_speed(uint8_t s)
{
    static const char *const names[] = { "?", "FS", "LS", "HS", "SS", "SS+" };
    return s < 6 ? names[s] : "?";
}

/* " if0 03.01.01 kbd" for each interface but a hub's. */
static void interfaces(handle_t bus, uint32_t id, uint8_t nifs)
{
    for (uint8_t k = 0; k < nifs && k < 8; k++) {
        uint8_t num, alt, nalt, icls, isub, iproto, nep, eps[8];
        if (usbbus_interface_until(bus, (uint64_t)jam_clock_get() + 2 * SH_S, id, k, &num, &alt,
                                   &nalt, &icls, &isub, &iproto, &nep, eps) != OK)
            continue;
        const char *what = icls == 3 && isub == 1 && iproto == 1 ? " kbd"
                         : icls == 3 && isub == 1 && iproto == 2 ? " mouse"
                         : icls == 3 ? " hid" : icls == 8 ? " storage" : icls == 9 ? "" : "";
        if (icls != 9)
            sh_say(" if%u %02x.%02x.%02x%s", num, icls, isub, iproto, what);
    }
}

/* Device i's line; false if usb-bus has no device i. */
static bool device_line(handle_t bus, uint32_t i, struct seen *seen, uint32_t *nseen)
{
    uint32_t id, parent, route;
    uint16_t vendor, product, bcd, mp0;
    uint8_t speed, addr, slot, rport, port, level, tts, ttp, cls, sub, proto, ncfg, cfg, nifs,
        hubports, path[24], name[40], serial[24];
    if (usbbus_device_until(bus, (uint64_t)jam_clock_get() + 2 * SH_S, i, &id, &parent, &vendor,
                            &product, &bcd, &speed, &addr, &slot, &rport, &port, &level, &route,
                            &tts, &ttp, &cls, &sub, &proto, &ncfg, &cfg, &nifs, &mp0, &hubports,
                            path, name, serial) != OK)
        return false;
    path[23] = name[39] = '\0';
    if (*nseen < MAX_SEEN)
        seen[(*nseen)++] = (struct seen){ id, vendor, product };
    sh_say("  %-8s %04x:%04x %-3s", (char *)path, vendor, product, usb_speed(speed));
    if (cls == 9)
        sh_say(" hub, %u ports", hubports);
    interfaces(bus, id, nifs);
    for (uint32_t j = 0; parent && j < *nseen; j++)
        if (seen[j].id == parent)
            sh_say(" behind hub %04x:%04x", seen[j].vendor, seen[j].product);
    if (name[0])
        sh_say("  \"%s\"", (char *)name);
    sh_say("\n");
    return true;
}

SH_CMD(usb)
{
    (void)argc;
    (void)argv;
    handle_t bus = find_usb_bus();
    if (!bus) {
        sh_say("usb: no USB bus driver bound (devmgr has none that answers usbbus)\n");
        return 0;
    }
    uint32_t ndev, nhub, nif, nhid, nprob, gen;
    uint8_t settled;
    status_t st = usbbus_wait_settled_until(bus, (uint64_t)jam_clock_get() + 5 * SH_S, 3000, &ndev,
                                            &nhub, &nif, &nhid, &nprob, &gen, &settled);
    if (st != OK) {
        sh_say("usb: %s\n", status_str(st));
        jam_handle_close(bus);
        return 0;
    }
    sh_say("usb: %u device(s), %u hub(s), %u interface(s) (%u HID), %u problem(s)%s\n", ndev, nhub,
           nif, nhid, nprob, settled ? "" : ", still settling");
    struct seen seen[MAX_SEEN];
    uint32_t nseen = 0;
    for (uint32_t i = 0; i < ndev && device_line(bus, i, seen, &nseen); i++)
        ;
    jam_handle_close(bus);
    return 0;
}
