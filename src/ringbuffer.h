/*
 * Single-producer / single-consumer lock-free ring buffer.
 *
 * The producer is an interrupt handler (here: the simulated CAN/UART ISR), the
 * consumer is a task. With exactly one writer per index, no lock or critical
 * section is needed: the producer only writes `head`, the consumer only
 * writes `tail`, and acquire/release ordering makes the element data visible
 * before the index that publishes it.
 *
 * Capacity must be a power of two; indices are free-running 32-bit counters
 * (wrap-around is handled by unsigned arithmetic), so all slots are usable.
 */
#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *storage;
    size_t elem_size;
    uint32_t capacity;           /* power of two */
    volatile uint32_t head;      /* written by producer only */
    volatile uint32_t tail;      /* written by consumer only */
    volatile uint32_t dropped;   /* producer-side: elements rejected because full */
} ringbuffer_t;

bool rb_init(ringbuffer_t *rb, void *storage, size_t elem_size, uint32_t capacity);
bool rb_put(ringbuffer_t *rb, const void *elem);   /* ISR side, never blocks */
bool rb_get(ringbuffer_t *rb, void *elem);         /* task side */
uint32_t rb_count(const ringbuffer_t *rb);

#endif
