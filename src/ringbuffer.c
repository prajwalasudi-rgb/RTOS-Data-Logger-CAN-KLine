#include "ringbuffer.h"

#include <string.h>

bool rb_init(ringbuffer_t *rb, void *storage, size_t elem_size, uint32_t capacity)
{
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
        return false;   /* must be a power of two */
    }
    rb->storage = storage;
    rb->elem_size = elem_size;
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->dropped = 0;
    return true;
}

uint32_t rb_count(const ringbuffer_t *rb)
{
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    return head - tail;
}

bool rb_put(ringbuffer_t *rb, const void *elem)
{
    uint32_t head = rb->head;
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    if (head - tail >= rb->capacity) {
        rb->dropped++;
        return false;
    }
    memcpy(rb->storage + (size_t)(head & (rb->capacity - 1)) * rb->elem_size, elem, rb->elem_size);
    __atomic_store_n(&rb->head, head + 1, __ATOMIC_RELEASE);   /* publish after the data */
    return true;
}

bool rb_get(ringbuffer_t *rb, void *elem)
{
    uint32_t tail = rb->tail;
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    if (head == tail) {
        return false;
    }
    memcpy(elem, rb->storage + (size_t)(tail & (rb->capacity - 1)) * rb->elem_size, rb->elem_size);
    __atomic_store_n(&rb->tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}
