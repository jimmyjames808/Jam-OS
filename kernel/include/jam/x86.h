#pragma once

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }
static inline void cpu_relax(void) { __asm__ volatile("pause"); }
