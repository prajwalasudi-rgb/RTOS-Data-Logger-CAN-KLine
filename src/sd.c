/*
 * SD card + FAT file system model. Data really is written to files on the
 * host; the time a real card would need is added on top:
 *
 *  sd_append_sync_blocking()  small append + f_sync, CPU busy-waits (polling
 *                             SDIO driver): nothing else runs meanwhile.
 *  sd_write_sector_dma()      full-sector write by DMA: the calling task
 *                             blocks, the CPU is free for other tasks/ISRs.
 */
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"
#include "model.h"

static uint64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void busy_wait_us(unsigned us)
{
    uint64_t end = mono_us() + us;
    while (mono_us() < end) { /* polling the card, CPU blocked */ }
}

static void task_wait_us(unsigned us)
{
    static unsigned debt;          /* sub-tick remainder carried over */
    debt += us;
    TickType_t ticks = debt / 1000u;
    debt -= ticks * 1000u;
    if (ticks) vTaskDelay(ticks);
}

int sd_open(sd_file_t *f, const char *name)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_cfg.out_dir, name);
    f->name = name;
    f->fp = fopen(path, "w");
    return f->fp ? 0 : -1;
}

void sd_append_sync_blocking(sd_file_t *f, const char *data, size_t len)
{
    fwrite(data, 1, len, f->fp);
    fflush(f->fp);
    busy_wait_us(g_cfg.sd_append_sync_us);
    g_stats.bytes_written += len;
    g_stats.sd_busy_us += g_cfg.sd_append_sync_us;
    g_stats.sd_writes++;
    g_stats.sd_syncs++;
}

void sd_write_sector_dma(sd_file_t *f, const char *data, size_t len)
{
    fwrite(data, 1, len, f->fp);
    task_wait_us(g_cfg.sd_sector_us);
    g_stats.bytes_written += len;
    g_stats.sd_busy_us += g_cfg.sd_sector_us;
    g_stats.sd_writes++;
}

void sd_sync(sd_file_t *f)
{
    fflush(f->fp);
    task_wait_us(g_cfg.sd_sync_us);
    g_stats.sd_busy_us += g_cfg.sd_sync_us;
    g_stats.sd_syncs++;
}

void sd_close(sd_file_t *f)
{
    if (f->fp) fclose(f->fp);
    f->fp = NULL;
}
