/* port_again (<os.h>): another turn for a channel a loop stopped reading
 * early. A PERSISTENT binding is edge-triggered (kernel/object/port.c): it
 * queues a packet when the signals it watches turn to match, and nothing
 * while they stay so. A loop that reads at most N messages a turn and
 * leaves the rest queued gets no packet for them, and none for anything
 * that comes after either: the channel never turns readable again while
 * it is readable. So it asks for its next turn itself: a user packet with
 * the binding's key, or, when the port takes none, the binding made again
 * (a new binding fires at once if its signals already match). */
#include <os.h>

status_t port_again(handle_t port, handle_t h, uint64_t key, signals_t mask)
{
    struct port_packet p = { .key = key, .type = PORT_PACKET_USER };
    if (jam_port_queue(port, &p) == OK)
        return OK;
    (void)jam_port_unbind(port, h, key);   /* its charge back first: the bind takes it again */
    return jam_port_bind(port, h, key, mask, PORT_BIND_PERSISTENT);
}
