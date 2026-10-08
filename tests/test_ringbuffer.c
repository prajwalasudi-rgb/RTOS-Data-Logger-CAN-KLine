/* Ring buffer tests: basic behaviour, full/empty, wrap-around of the 32-bit
 * indices, and a two-thread stress test (one producer, one consumer). */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

#include "ringbuffer.h"

static void test_basic(void)
{
    uint32_t store[4], v;
    ringbuffer_t rb;
    assert(!rb_init(&rb, store, sizeof v, 3));           /* not a power of two */
    assert(rb_init(&rb, store, sizeof v, 4));
    assert(!rb_get(&rb, &v));                            /* empty */
    for (uint32_t i = 0; i < 4; i++) assert(rb_put(&rb, &i));
    v = 99;
    assert(!rb_put(&rb, &v) && rb.dropped == 1);         /* full: all 4 slots usable */
    for (uint32_t i = 0; i < 4; i++) { assert(rb_get(&rb, &v)); assert(v == i); }
    assert(rb_count(&rb) == 0);
}

static void test_index_wraparound(void)
{
    uint32_t store[8], v;
    ringbuffer_t rb;
    rb_init(&rb, store, sizeof v, 8);
    rb.head = rb.tail = 0xFFFFFFF0u;                      /* close to 2^32 */
    for (uint32_t i = 0; i < 100; i++) {
        assert(rb_put(&rb, &i));
        assert(rb_get(&rb, &v) && v == i);
    }
}

#define N 2000000u
static uint32_t big[1024];
static ringbuffer_t shared;

static void *producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < N; i++) while (!rb_put(&shared, &i)) { shared.dropped = 0; }
    return NULL;
}

static void test_spsc_stress(void)
{
    rb_init(&shared, big, sizeof(uint32_t), 1024);
    pthread_t t;
    pthread_create(&t, NULL, producer, NULL);
    uint32_t expect = 0, v;
    while (expect < N) {
        if (rb_get(&shared, &v)) { assert(v == expect); expect++; }
    }
    pthread_join(t, NULL);
}

int main(void)
{
    test_basic();
    test_index_wraparound();
    test_spsc_stress();
    puts("ringbuffer: all tests passed");
    return 0;
}
