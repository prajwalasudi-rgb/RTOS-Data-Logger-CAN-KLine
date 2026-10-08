/*
 * Bus model: a highest-priority task that plays the CAN controller and the
 * K-Line UART. Frames/bytes arrive in real time at the configured rates; the
 * hardware buffers are as small as on the real chip, and the "interrupt
 * handler" is called whenever data is waiting. Whatever arrives while the
 * hardware buffer is full is lost, exactly like an overrun on the MCU.
 */
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "model.h"

static can_isr_t s_can_isr;
static kline_isr_t s_kline_isr;
static volatile int s_running;
static TaskHandle_t s_task;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint32_t now_ms(void) { return (uint32_t)(now_us() / 1000u); }

static uint32_t lcg(void)
{
    static uint32_t s = 12345u;
    s = s * 1664525u + 1013904223u;
    return s;
}

static void make_frame(can_frame_t *f)
{
    static const uint32_t ids[] = {0x0CF00400, 0x18FEF100, 0x18FEEE00, 0x18FECA00, 0x0C000003};
    static unsigned n;
    f->timestamp_ms = now_ms();
    f->id = ids[n++ % (sizeof ids / sizeof ids[0])];
    f->dlc = 8;
    for (int i = 0; i < 8; i++) f->data[i] = (uint8_t)(lcg() >> 24);
}

/* Serve one item waiting in a hardware buffer. When both buses have data
 * waiting, they take turns (equal interrupt priority). */
static void serve_pending(can_frame_t *can_hw, unsigned *can_n, kline_byte_t *uart, int *uart_full)
{
    static int turn;
    int take_can = *can_n && (!*uart_full || (turn ^= 1));
    if (take_can) {
        s_can_isr(&can_hw[0]);
        memmove(can_hw, can_hw + 1, --(*can_n) * sizeof can_hw[0]);
    } else if (*uart_full) {
        s_kline_isr(uart);
        *uart_full = 0;
    }
}

/*
 * Event timing: every frame/byte has an arrival time. If the CPU is free at
 * that moment the interrupt handler runs immediately (its real execution time
 * is measured). Data that arrives while a handler is still running waits in
 * the hardware buffer (CAN FIFO: 3 frames, UART: 1 byte) and is lost when that
 * buffer is full, exactly like an overrun on the MCU. CAN and K-Line share one
 * CPU, so a slow handler for one bus also blocks the other.
 */
static void bus_task(void *arg)
{
    (void)arg;
    can_frame_t can_hw[8];
    unsigned can_hw_n = 0;
    kline_byte_t uart_hw;
    int uart_full = 0;
    const double can_dt = g_cfg.can_rate > 0 ? 1e6 / g_cfg.can_rate : 1e18;
    const double kl_dt = g_cfg.kline_rate > 0 ? 1e6 / g_cfg.kline_rate : 1e18;
    double t0 = (double)now_us();
    double next_can = t0 + can_dt, next_kl = t0 + kl_dt;
    double cpu_free_at = t0;   /* when the last interrupt handler returned */
    const double t_end = t0 + g_cfg.duration_s * 1e6;   /* the bus stops by itself, even if a
                                                          slow handler starves all tasks */

    while (s_running) {
        double now = (double)now_us();
        if (now >= t_end) break;
        while ((next_can <= now || next_kl <= now) && (double)now_us() < t_end) {
            int is_can = next_can <= next_kl;
            double ta = is_can ? next_can : next_kl;
            if (is_can) { next_can += can_dt; g_stats.can_generated++; }
            else        { next_kl += kl_dt;   g_stats.kline_generated++; }

            if (ta < cpu_free_at) {
                /* CPU still inside a handler: data waits in the hardware buffer */
                if (is_can) {
                    if (can_hw_n < g_cfg.can_fifo_depth) make_frame(&can_hw[can_hw_n++]);
                    else g_stats.can_fifo_overruns++;
                } else {
                    if (uart_full) g_stats.kline_overruns++;
                    uart_hw.timestamp_ms = now_ms();
                    uart_hw.byte = (uint8_t)(lcg() >> 24);
                    uart_full = 1;
                }
                continue;
            }
            /* CPU free: the interrupt fires now. Afterwards, pending hardware data
               triggers the handler again right away (interrupt still pending). */
            double start = ta, t_in = (double)now_us();
            if (is_can) { can_frame_t f; make_frame(&f); s_can_isr(&f); }
            else { kline_byte_t b = { now_ms(), (uint8_t)(lcg() >> 24) }; s_kline_isr(&b); }
            double busy = (double)now_us() - t_in;
            cpu_free_at = start + busy;
            /* (pending data that arrived during this handler is served below) */
            if (can_hw_n || uart_full) {   /* one pending item per step, then new arrivals */
                double t1 = (double)now_us();
                serve_pending(can_hw, &can_hw_n, &uart_hw, &uart_full);
                cpu_free_at += (double)now_us() - t1;
            }
        }
        /* serve anything still pending in the hardware buffers */
        if (can_hw_n || uart_full) {
            serve_pending(can_hw, &can_hw_n, &uart_hw, &uart_full);
            cpu_free_at = (double)now_us();
        }
        if (!can_hw_n && !uart_full && next_can > (double)now_us() && next_kl > (double)now_us())
            vTaskDelay(1);
    }
    /* data still waiting in the hardware buffers when logging stops is handled too */
    while (can_hw_n || uart_full) serve_pending(can_hw, &can_hw_n, &uart_hw, &uart_full);
    s_running = 0;
    vTaskDelete(NULL);
}

void bus_start(can_isr_t can_isr, kline_isr_t kline_isr)
{
    s_can_isr = can_isr;
    s_kline_isr = kline_isr;
    s_running = 1;
    configASSERT(xTaskCreate(bus_task, "bus", configMINIMAL_STACK_SIZE * 2, NULL, configMAX_PRIORITIES - 2, &s_task) == pdPASS);
}

void bus_stop(void) { s_running = 0; }

int format_can_line(char *out, size_t cap, const can_frame_t *fr)
{
    return snprintf(out, cap, "%10.3f 1  %08Xx  Rx d %u %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    fr->timestamp_ms / 1000.0, (unsigned)fr->id, fr->dlc,
                    fr->data[0], fr->data[1], fr->data[2], fr->data[3],
                    fr->data[4], fr->data[5], fr->data[6], fr->data[7]);
}
