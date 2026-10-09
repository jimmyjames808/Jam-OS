/* serialin: COM1 as an input source for the compositor.
 *
 * Reads what arrives on the serial port (the kernel's serial_open object,
 * interrupt-driven) and passes it on as `input.text` events (abi/idl/
 * input.idl) on the channel init got for it from compctl.connect_input:
 * the same path a HID driver's keys take. So a serial terminal, or a QEMU
 * test writing to the serial port, can type into the focused window, and
 * so into the shell. An event the compositor's full queue refuses is sent
 * again once there is room (serialin serves nobody else, so it waits):
 * nothing typed is lost.
 *
 * Startup handles:
 *   SR_RESOURCE   the root resource with RIGHT_ROOT_SERIAL (serial_open)
 *   SR_USER + 0   the `input` channel to the compositor
 *
 * Exits 0 when the other end closes the channel (it restarted: init
 * starts a new serialin for the new one), 1 if the port can't be opened. */
#include <idl/input.h>
#include <os.h>

#define K_SERIAL 1
#define K_COMP   2

/* n bytes as one text event, waiting for room: OK, or ERR_PEER_CLOSED. */
static status_t send_text(handle_t in, const uint8_t *buf, uint32_t n)
{
    for (;;) {
        status_t st = input_text(in, buf, n);
        if (st != ERR_SHOULD_WAIT)
            return st == ERR_PEER_CLOSED ? st : OK;   /* another error: those bytes are lost */
        signals_t seen = 0;
        st = jam_object_wait_one(in, SIG_WRITABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
        if (st != OK || ((seen & SIG_PEER_CLOSED) && !(seen & SIG_WRITABLE)))
            return ERR_PEER_CLOSED;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t root = startup_handle(SR_RESOURCE), in = startup_handle(SR_USER + 0);
    handle_t serial, port;
    if (!in) {
        printf("serialin: no input channel\n");
        return 1;
    }
    status_t st = jam_serial_open(root, &serial);
    if (st != OK) {
        printf("serialin: can't open COM1 (%s)\n", status_str(st));
        return 1;
    }
    if ((st = jam_port_create(&port)) != OK ||
        (st = jam_port_bind(port, serial, K_SERIAL, SIG_READABLE, PORT_BIND_PERSISTENT)) != OK ||
        (st = jam_port_bind(port, in, K_COMP, SIG_PEER_CLOSED, PORT_BIND_ONCE)) != OK) {
        printf("serialin: port: %s\n", status_str(st));
        return 1;
    }
    for (;;) {
        struct port_packet pkt;
        st = jam_port_wait(port, DEADLINE_NEVER, &pkt);
        if (st != OK)
            return 1;
        if (pkt.key == K_COMP)
            return 0;
        uint8_t buf[64];
        int64_t n;
        while ((n = jam_serial_read(serial, buf, sizeof(buf))) > 0)
            if (send_text(in, buf, (uint32_t)n) == ERR_PEER_CLOSED)
                return 0;
    }
}
