/*
 * RTOS data logger for CAN and K-Line, running on the FreeRTOS POSIX port.
 *
 *   --mode sequential   prototype design: the interrupt handler formats each
 *                       message and writes it to the SD card itself
 *   --mode rtos         redesign: ISRs only push into lock-free ring buffers;
 *                       a producer task formats messages into 512-byte sector
 *                       buffers, a storage task writes full sectors by DMA,
 *                       a main task runs the state machine
 *
 * Usage: rtos_logger [--mode rtos|sequential] [--can-rate N] [--kline-rate N]
 *                    [--duration S] [--out DIR]
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "model.h"
#include "ringbuffer.h"

sim_config_t g_cfg = {
    .mode = MODE_RTOS,
    .can_rate = 1000.0,
    .kline_rate = 945.0,
    .duration_s = 3.0,
    .can_fifo_depth = 3,
    .sd_append_sync_us = 2500,
    .sd_sector_us = 1200,
    .sd_sync_us = 6000,
    .sync_every = 16,
    .out_dir = "logs",
};
sim_stats_t g_stats;

/* ------------------------------------------------------------- tasks ---- */
#define PRIO_STORAGE  (tskIDLE_PRIORITY + 1)
#define PRIO_PRODUCER (tskIDLE_PRIORITY + 2)
#define PRIO_MAIN     (tskIDLE_PRIORITY + 3)
/* bus model ("hardware" + ISRs) runs above all of these */

#define SECTOR 512
#define N_SECTOR_BUFS 16
#define CAN_RING 1024
#define KLINE_RING 2048

typedef enum { FILE_CAN = 0, FILE_KLINE = 1 } file_id_t;

typedef struct {
    file_id_t file;
    uint16_t len;
    uint16_t records;
    char data[SECTOR];
} sector_buf_t;

static sd_file_t s_files[2];
static ringbuffer_t s_can_rb, s_kline_rb;
static can_frame_t s_can_store[CAN_RING];
static kline_byte_t s_kline_store[KLINE_RING];
static sector_buf_t s_bufs[N_SECTOR_BUFS];
static QueueHandle_t s_free_q, s_full_q;     /* buffer pool: free -> producer -> full -> storage */
static volatile int s_stop_producer, s_producer_done, s_storage_idle;

/* ------------------------------------------------ sequential prototype -- */
static void seq_can_isr(const can_frame_t *fr)
{
    char line[96];
    int n = format_can_line(line, sizeof line, fr);
    sd_append_sync_blocking(&s_files[FILE_CAN], line, (size_t)n);   /* inside the ISR */
    g_stats.can_logged++;
}

static void seq_kline_isr(const kline_byte_t *b)
{
    char line[32];
    int n = snprintf(line, sizeof line, "%10.3f %02X\n", b->timestamp_ms / 1000.0, b->byte);
    sd_append_sync_blocking(&s_files[FILE_KLINE], line, (size_t)n);
    g_stats.kline_logged++;
}

/* --------------------------------------------------------- RTOS design -- */
static void rtos_can_isr(const can_frame_t *fr)
{
    if (!rb_put(&s_can_rb, fr)) g_stats.can_buffer_drops++;   /* O(1), never blocks */
}

static void rtos_kline_isr(const kline_byte_t *b)
{
    if (!rb_put(&s_kline_rb, b)) g_stats.kline_buffer_drops++;
}

static void submit(sector_buf_t **cur)
{
    if (*cur && (*cur)->len) {
        xQueueSend(s_full_q, cur, portMAX_DELAY);
        *cur = NULL;
    }
}

static int append(sector_buf_t **cur, file_id_t file, const char *line, int n)
{
    if (*cur && (*cur)->len + n > SECTOR) submit(cur);
    if (!*cur) {
        if (xQueueReceive(s_free_q, cur, pdMS_TO_TICKS(50)) != pdPASS) return 0;  /* pool empty */
        (*cur)->file = file;
        (*cur)->len = 0;
        (*cur)->records = 0;
    }
    memcpy((*cur)->data + (*cur)->len, line, (size_t)n);
    (*cur)->len += (uint16_t)n;
    (*cur)->records++;
    return 1;
}

/* Producer: drain ring buffers, format records, fill sector buffers. */
static void producer_task(void *arg)
{
    (void)arg;
    sector_buf_t *can_buf = NULL, *kl_buf = NULL;
    TickType_t last_flush = xTaskGetTickCount();
    char line[96];
    for (;;) {
        int work = 0;
        can_frame_t fr;
        while (rb_get(&s_can_rb, &fr)) {
            int n = format_can_line(line, sizeof line, &fr);
            work += append(&can_buf, FILE_CAN, line, n);
        }
        kline_byte_t kb;
        while (rb_get(&s_kline_rb, &kb)) {
            int n = snprintf(line, sizeof line, "%10.3f %02X\n", kb.timestamp_ms / 1000.0, kb.byte);
            work += append(&kl_buf, FILE_KLINE, line, n);
        }
        /* do not keep partial sectors forever: flush every 500 ms */
        if (xTaskGetTickCount() - last_flush > pdMS_TO_TICKS(500)) {
            submit(&can_buf);
            submit(&kl_buf);
            last_flush = xTaskGetTickCount();
        }
        if (s_stop_producer && rb_count(&s_can_rb) == 0 && rb_count(&s_kline_rb) == 0) {
            submit(&can_buf);
            submit(&kl_buf);
            s_producer_done = 1;
            vTaskDelete(NULL);
        }
        if (!work) vTaskDelay(1);
    }
}

/* Storage: write full sectors (DMA), f_sync periodically, recycle buffers. */
static void storage_task(void *arg)
{
    (void)arg;
    unsigned since_sync = 0;
    sector_buf_t *b;
    for (;;) {
        if (xQueueReceive(s_full_q, &b, pdMS_TO_TICKS(10)) == pdPASS) {
            s_storage_idle = 0;
            sd_write_sector_dma(&s_files[b->file], b->data, b->len);
            if (b->file == FILE_CAN) g_stats.can_logged += b->records;
            else g_stats.kline_logged += b->records;
            xQueueSend(s_free_q, &b, portMAX_DELAY);
            if (++since_sync >= g_cfg.sync_every) {
                sd_sync(&s_files[FILE_CAN]);
                sd_sync(&s_files[FILE_KLINE]);
                since_sync = 0;
            }
        } else {
            s_storage_idle = 1;
        }
    }
}

/* ------------------------------------------------- main state machine --- */
typedef enum { ST_INIT, ST_MOUNT, ST_OPEN, ST_LOGGING, ST_STOPPING, ST_CLOSE, ST_REPORT } state_t;
static const char *state_name[] = {"INIT", "MOUNT", "OPEN", "LOGGING", "STOPPING", "CLOSE", "REPORT"};

static void report(void)
{
    double can_pct = g_stats.can_generated ? 100.0 * g_stats.can_logged / g_stats.can_generated : 0;
    double kl_pct = g_stats.kline_generated ? 100.0 * g_stats.kline_logged / g_stats.kline_generated : 0;
    const char *mode = g_cfg.mode == MODE_RTOS ? "rtos" : "sequential";
    printf("\n=== %s logger, CAN %.0f frames/s, K-Line %.0f bytes/s, %.1f s ===\n",
           mode, g_cfg.can_rate, g_cfg.kline_rate, g_cfg.duration_s);
    printf("CAN     received %8llu  logged %8llu  (%5.1f %%)  lost: FIFO overrun %llu, buffer full %llu\n",
           (unsigned long long)g_stats.can_generated, (unsigned long long)g_stats.can_logged, can_pct,
           (unsigned long long)g_stats.can_fifo_overruns, (unsigned long long)g_stats.can_buffer_drops);
    printf("K-Line  received %8llu  logged %8llu  (%5.1f %%)  lost: UART overrun %llu, buffer full %llu\n",
           (unsigned long long)g_stats.kline_generated, (unsigned long long)g_stats.kline_logged, kl_pct,
           (unsigned long long)g_stats.kline_overruns, (unsigned long long)g_stats.kline_buffer_drops);
    printf("SD      %llu writes, %llu syncs, %llu bytes\n", (unsigned long long)g_stats.sd_writes,
           (unsigned long long)g_stats.sd_syncs, (unsigned long long)g_stats.bytes_written);

    char path[512];
    snprintf(path, sizeof path, "%s/stats.json", g_cfg.out_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "{\"mode\": \"%s\", \"can_rate\": %.0f, \"kline_rate\": %.0f, \"duration_s\": %.2f,\n"
                   " \"can_received\": %llu, \"can_logged\": %llu, \"can_logged_pct\": %.2f,\n"
                   " \"can_fifo_overruns\": %llu, \"can_buffer_drops\": %llu,\n"
                   " \"kline_received\": %llu, \"kline_logged\": %llu, \"kline_logged_pct\": %.2f,\n"
                   " \"kline_overruns\": %llu, \"kline_buffer_drops\": %llu,\n"
                   " \"sd_writes\": %llu, \"sd_syncs\": %llu, \"bytes_written\": %llu}\n",
                mode, g_cfg.can_rate, g_cfg.kline_rate, g_cfg.duration_s,
                (unsigned long long)g_stats.can_generated, (unsigned long long)g_stats.can_logged, can_pct,
                (unsigned long long)g_stats.can_fifo_overruns, (unsigned long long)g_stats.can_buffer_drops,
                (unsigned long long)g_stats.kline_generated, (unsigned long long)g_stats.kline_logged, kl_pct,
                (unsigned long long)g_stats.kline_overruns, (unsigned long long)g_stats.kline_buffer_drops,
                (unsigned long long)g_stats.sd_writes, (unsigned long long)g_stats.sd_syncs,
                (unsigned long long)g_stats.bytes_written);
        fclose(f);
    }
}

static void main_task(void *arg)
{
    (void)arg;
    state_t st = ST_INIT;
    TickType_t t_start = 0;
    for (;;) {
        switch (st) {
        case ST_INIT:
            st = ST_MOUNT;
            break;
        case ST_MOUNT:   /* f_mount on the target; here: make sure the output folder exists */
            if (mkdir(g_cfg.out_dir, 0755) != 0 && errno != EEXIST) {
                printf("[main] cannot create %s\n", g_cfg.out_dir);
                exit(2);
            }
            st = ST_OPEN;
            break;
        case ST_OPEN:
            if (sd_open(&s_files[FILE_CAN], "can.asc") || sd_open(&s_files[FILE_KLINE], "kline.log")) {
                printf("[main] cannot open log files\n");
                exit(2);
            }
            fputs("date logged by rtos-data-logger\nbase hex  timestamps absolute\n", s_files[FILE_CAN].fp);
            if (g_cfg.mode == MODE_RTOS) {
                configASSERT(xTaskCreate(producer_task, "producer", configMINIMAL_STACK_SIZE * 2, NULL, PRIO_PRODUCER, NULL) == pdPASS);
                configASSERT(xTaskCreate(storage_task, "storage", configMINIMAL_STACK_SIZE * 2, NULL, PRIO_STORAGE, NULL) == pdPASS);
                bus_start(rtos_can_isr, rtos_kline_isr);
            } else {
                bus_start(seq_can_isr, seq_kline_isr);
            }
            t_start = xTaskGetTickCount();
            st = ST_LOGGING;
            break;
        case ST_LOGGING:
            vTaskDelay(pdMS_TO_TICKS(100));
            if (xTaskGetTickCount() - t_start >= pdMS_TO_TICKS((uint32_t)(g_cfg.duration_s * 1000))) {
                bus_stop();
                s_stop_producer = 1;
                st = ST_STOPPING;
            }
            break;
        case ST_STOPPING:   /* drain everything that was received before the stop */
            vTaskDelay(pdMS_TO_TICKS(20));
            if (g_cfg.mode == MODE_SEQUENTIAL ||
                (s_producer_done && uxQueueMessagesWaiting(s_full_q) == 0 && s_storage_idle)) {
                st = ST_CLOSE;
            }
            break;
        case ST_CLOSE:
            if (g_cfg.mode == MODE_RTOS) {
                sd_sync(&s_files[FILE_CAN]);
                sd_sync(&s_files[FILE_KLINE]);
            }
            sd_close(&s_files[FILE_CAN]);
            sd_close(&s_files[FILE_KLINE]);
            st = ST_REPORT;
            break;
        case ST_REPORT:
            report();
            exit(0);
        }
        (void)state_name;
    }
}

/* -------------------------------------------------------------- setup --- */
void vAssertCalled(const char *file, unsigned long line)
{
    printf("ASSERT %s:%lu\n", file, line);
    abort();
}

static void usage(void)
{
    puts("usage: rtos_logger [--mode rtos|sequential] [--can-rate N] [--kline-rate N] "
         "[--duration S] [--out DIR]");
    exit(1);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "rtos")) g_cfg.mode = MODE_RTOS;
            else if (!strcmp(m, "sequential")) g_cfg.mode = MODE_SEQUENTIAL;
            else usage();
        } else if (!strcmp(argv[i], "--can-rate") && i + 1 < argc) g_cfg.can_rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--kline-rate") && i + 1 < argc) g_cfg.kline_rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc) g_cfg.duration_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) g_cfg.out_dir = argv[++i];
        else usage();
    }

    rb_init(&s_can_rb, s_can_store, sizeof(can_frame_t), CAN_RING);
    rb_init(&s_kline_rb, s_kline_store, sizeof(kline_byte_t), KLINE_RING);
    s_free_q = xQueueCreate(N_SECTOR_BUFS, sizeof(sector_buf_t *));
    s_full_q = xQueueCreate(N_SECTOR_BUFS, sizeof(sector_buf_t *));
    for (int i = 0; i < N_SECTOR_BUFS; i++) {
        sector_buf_t *p = &s_bufs[i];
        xQueueSend(s_free_q, &p, 0);
    }
    configASSERT(xTaskCreate(main_task, "main", configMINIMAL_STACK_SIZE * 2, NULL, PRIO_MAIN, NULL) == pdPASS);
    vTaskStartScheduler();
    return 0;
}
