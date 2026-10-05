// SPDX-License-Identifier: (MIT OR Apache-2.0)
/*
 * Block device I/O library
 * Copyright (C) 2020 Red Hat, Inc.
 *
 * See blkio(3) for API documentation.
 */
#ifndef BLKIO_H
#define BLKIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <sys/uio.h>

struct blkio;
struct blkioq;

struct blkio_mem_region
{
    void *addr;
    size_t len;
    uint64_t iova;
    int64_t fd_offset;
    int fd;
    uint32_t flags;
};

struct blkio_completion
{
    void *user_data;
    const char *error_msg;
    int ret;
    uint8_t reserved_[12];
};

const char *blkio_get_error_msg(void);

int blkio_create(const char *driver, struct blkio **bp);
int blkio_connect(struct blkio *b);
int blkio_start(struct blkio *b);
void blkio_destroy(struct blkio **bp);

int blkio_get_bool(struct blkio *b, const char *name, bool *value);
int blkio_get_int(struct blkio *b, const char *name, int *value);
int blkio_get_uint64(struct blkio *b, const char *name, uint64_t *value);
int blkio_get_str(struct blkio *b, const char *name, char **value);

int blkio_set_bool(struct blkio *b, const char *name, bool value);
int blkio_set_int(struct blkio *b, const char *name, int value);
int blkio_set_uint64(struct blkio *b, const char *name, uint64_t value);
int blkio_set_str(struct blkio *b, const char *name, const char *value);

int blkio_alloc_mem_region(struct blkio *b, struct blkio_mem_region *region, size_t len);
void blkio_free_mem_region(struct blkio *b, const struct blkio_mem_region *region);
int blkio_map_mem_region(struct blkio *b, const struct blkio_mem_region *region);
void blkio_unmap_mem_region(struct blkio *b, const struct blkio_mem_region *region);

struct blkioq *blkio_get_queue(struct blkio *b, int index);
struct blkioq *blkio_get_poll_queue(struct blkio *b, int index);

int blkio_add_queue(struct blkio *b);
int blkio_add_poll_queue(struct blkio *b);

int blkio_remove_queue(struct blkio *b, int index);
int blkio_remove_poll_queue(struct blkio *b, int index);

enum {
    BLKIO_REQ_FUA = 1 << 0,
    BLKIO_REQ_NO_UNMAP = 1 << 1,
    BLKIO_REQ_NO_FALLBACK = 1 << 2,
};

void blkioq_read(struct blkioq *q, uint64_t start, void *buf, size_t len, void *user_data, uint32_t flags);
void blkioq_write(struct blkioq *q, uint64_t start, const void *buf, size_t len, void *user_data, uint32_t flags);
void blkioq_readv(struct blkioq *q, uint64_t start, const struct iovec *iovec, int iovcnt, void *user_data, uint32_t flags);
void blkioq_writev(struct blkioq *q, uint64_t start, struct iovec *iovec, int iovcnt, void *user_data, uint32_t flags);
void blkioq_write_zeroes(struct blkioq *q, uint64_t start, uint64_t len, void *user_data, uint32_t flags);
void blkioq_discard(struct blkioq *q, uint64_t start, uint64_t len, void *user_data, uint32_t flags);
void blkioq_flush(struct blkioq *q, void *user_data, uint32_t flags);

enum {
    BLKIO_ZONE_TYPE_CONVENTIONAL = 0x1,
    BLKIO_ZONE_TYPE_SEQWRITE_REQ = 0x2,
    BLKIO_ZONE_TYPE_SEQWRITE_PREF = 0x3,
};

enum {
    BLKIO_ZONE_STATE_EMPTY = 0x1,
    BLKIO_ZONE_STATE_IMP_OPEN = 0x2,
    BLKIO_ZONE_STATE_EXP_OPEN = 0x3,
    BLKIO_ZONE_STATE_CLOSED = 0x4,
    BLKIO_ZONE_STATE_READONLY = 0xD,
    BLKIO_ZONE_STATE_FULL = 0xE,
    BLKIO_ZONE_STATE_OFFLINE = 0xF,
};

struct blkio_zone {
    uint64_t start;
    uint64_t len;
    uint64_t capacity;
    uint64_t write_pointer;
    uint8_t  zone_type;
    uint8_t  zone_state;
    uint8_t  reset;
    uint8_t  reserved_[29];
};

void blkioq_report_zones(struct blkioq *q, uint64_t offset, struct blkio_zone *zones, uint32_t nr_zones, void *user_data, uint32_t flags);

void blkioq_close_zone(struct blkioq *q, uint64_t offset, void *user_data, uint32_t flags);
void blkioq_finish_zone(struct blkioq *q, uint64_t offset, void *user_data, uint32_t flags);
void blkioq_open_zone(struct blkioq *q, uint64_t offset, void *user_data, uint32_t flags);
void blkioq_reset_zone(struct blkioq *q, uint64_t offset, void *user_data, uint32_t flags);

void blkioq_close_all_zones(struct blkioq *q, void *user_data, uint32_t flags);
void blkioq_finish_all_zones(struct blkioq *q, void *user_data, uint32_t flags);
void blkioq_open_all_zones(struct blkioq *q, void *user_data, uint32_t flags);
void blkioq_reset_all_zones(struct blkioq *q, void *user_data, uint32_t flags);

int blkioq_do_io(struct blkioq *q, struct blkio_completion *completions, int min_completions, int max_completions, struct timespec *timeout);
int blkioq_do_io_interruptible(struct blkioq *q, struct blkio_completion *completions, int min_completions, int max_completions, struct timespec *timeout, const sigset_t *sig);
int blkioq_get_completion_fd(struct blkioq *q);
void blkioq_set_completion_fd_enabled(struct blkioq *q, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* BLKIO_H */
