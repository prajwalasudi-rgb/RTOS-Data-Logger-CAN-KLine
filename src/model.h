/*
 * Models of the hardware the logger runs on, and the shared configuration.
 *
 *  - CAN controller: frames arrive at a configurable rate; like the STM32 bxCAN,
 *    the receive FIFO holds only 3 frames. If the ISR does not empty it in time,
 *    further frames are lost (FIFO overrun).
 *  - K-Line UART (10400 baud): one-byte receive register; a byte that is not read
 *    before the next one arrives is lost (UART overrun).
 *  - SD card + FAT file system: writing costs time. Small appends followed by a
 *    sync are expensive (read-modify-write of a sector + FAT update), whole-sector
 *    writes are cheap per byte. All timings are configurable model parameters,
 *    not measurements of a specific card.
 */
#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>
#include <stdio.h>

typedef enum { MODE_SEQUENTIAL = 0, MODE_RTOS = 1 } logger_mode_t;

typedef struct {
    logger_mode_t mode;
    double can_rate;            /* CAN frames per second */
    double kline_rate;          /* K-Line bytes per second (10400 baud, 11 bit/byte ~ 945) */
    double duration_s;          /* logging time */
    unsigned can_fifo_depth;    /* bxCAN: 3 */
    /* SD card model (microseconds) */
    unsigned sd_append_sync_us; /* small f_write + f_sync of one record */
    unsigned sd_sector_us;      /* write of one full 512-byte sector (DMA) */
    unsigned sd_sync_us;        /* f_sync (FAT + directory update) */
    unsigned sync_every;        /* RTOS mode: f_sync after this many sectors */
    const char *out_dir;
} sim_config_t;

typedef struct {
    uint32_t timestamp_ms;
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
} can_frame_t;

typedef struct {
    uint32_t timestamp_ms;
    uint8_t byte;
} kline_byte_t;

typedef struct {
    /* bus side */
    volatile uint64_t can_generated, can_fifo_overruns;
    volatile uint64_t kline_generated, kline_overruns;
    /* logger side */
    volatile uint64_t can_buffer_drops, kline_buffer_drops;
    volatile uint64_t can_logged, kline_logged;
    volatile uint64_t bytes_written, sd_busy_us, sd_writes, sd_syncs;
} sim_stats_t;

extern sim_config_t g_cfg;
extern sim_stats_t g_stats;

/* bus model: the "hardware" task raises these "interrupts" */
typedef void (*can_isr_t)(const can_frame_t *frame);
typedef void (*kline_isr_t)(const kline_byte_t *b);
void bus_start(can_isr_t can_isr, kline_isr_t kline_isr);
void bus_stop(void);

/* SD card + file system model */
typedef struct { FILE *fp; const char *name; } sd_file_t;
int  sd_open(sd_file_t *f, const char *name);
void sd_append_sync_blocking(sd_file_t *f, const char *data, size_t len); /* CPU waits */
void sd_write_sector_dma(sd_file_t *f, const char *data, size_t len);     /* task waits, CPU free */
void sd_sync(sd_file_t *f);
void sd_close(sd_file_t *f);

/* helpers */
uint32_t now_ms(void);
int format_can_line(char *out, size_t cap, const can_frame_t *fr);   /* Vector ASC style */

#endif
