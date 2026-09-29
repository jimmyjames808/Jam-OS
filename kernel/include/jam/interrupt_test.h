/* Interrupt objects: hooks for ktests and the benchmark only (M6, Track
 * B). Nothing outside kernel/test uses these. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/interrupt.h>

struct job;

/* A virtual interrupt object charged to `job` (NULL: nobody). With
 * `maskable` it behaves like an MSI-X vector: the first fire masks it, a
 * fire while masked only sets a pending bit (the PBA), and interrupt_ack
 * delivers that pending fire when it unmasks. */
status_t interrupt_create_virtual_ex(struct job *job, bool maskable, struct kobject **out);
/* The (cpu, vector) the object owns: an IPI with that vector to that CPU
 * goes down the same path as its MSI. False once it is torn down. */
bool interrupt_vector_of(struct kobject *irq, uint32_t *cpu, uint8_t *vec);
/* Fires that arrived after teardown started (ignored), and whether the
 * object is masked right now. */
uint64_t interrupt_late_fires(struct kobject *irq);
bool interrupt_is_masked(struct kobject *irq);
/* Live interrupt objects (leak checks). */
uint64_t interrupt_live_count(void);
