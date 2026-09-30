/* hda: the verbs the driver may send to a codec (spec 7.3). Every command
 * to a codec goes through this file: hda_get takes GET verbs only, which
 * read and change nothing. ctrl.c's hda_command is the raw send, and
 * nothing but this file calls it, so the checks here are the whole list
 * of what the driver can ever ask a codec to do. */
#include "hda.h"

/* A GET verb: 12-bit verbs 0xf00-0xfff, or the 4-bit GET verbs 0xa
 * (converter format) and 0xb (amplifier gain/mute). Everything else sets
 * something in the codec (spec 7.3). */
static bool is_get(uint32_t verb, uint32_t payload)
{
    if (verb >= 0xf00 && verb <= 0xfff)
        return payload <= 0xff;
    return (verb == V4_GET_FORMAT || verb == V4_GET_AMP) && payload <= 0xffff;
}

/* The command word (spec 7.3): codec address, node, then a 12-bit verb
 * with an 8-bit payload or a 4-bit verb with a 16-bit payload. */
static uint32_t command(unsigned cad, unsigned nid, uint32_t verb, uint32_t payload)
{
    uint32_t cmd = (uint32_t)cad << 28 | (uint32_t)nid << 20;
    return cmd | (verb >= 0x100 ? verb << 8 | payload : verb << 16 | payload);
}

status_t hda_get(struct hda *h, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload,
                 uint32_t *out)
{
    if (cad >= HDA_MAX_CODECS || nid > 0x7f || !is_get(verb, payload))
        return ERR_INVALID_ARGS;
    return hda_command(h, cad, command(cad, nid, verb, payload), out);
}

status_t hda_param(struct hda *h, unsigned cad, unsigned nid, uint32_t param, uint32_t *out)
{
    return hda_get(h, cad, nid, V_GET_PARAM, param, out);
}
