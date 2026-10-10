
#include <blkio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLOCK_SIZE 4096
#define OFFSET (1024 * 1024)

static void check(int ret, const char *operation)
{
    if (ret != 0) {
        fprintf(stderr, "%s failed: %d (%s)\n",
                operation, ret, blkio_get_error_msg());
        exit(EXIT_FAILURE);
    }
}

static void wait_completion(struct blkioq *q, const char *operation)
{
    struct blkio_completion c = {0};
    struct timespec timeout = {
        .tv_sec = 5,
        .tv_nsec = 0,
    };

    int ret = blkioq_do_io(q, &c, 1, 1, &timeout);

    if (ret != 1) {
        fprintf(stderr, "%s: do_io returned %d: %s\n",
                operation, ret, blkio_get_error_msg());
        exit(EXIT_FAILURE);
    }

    if (c.ret != 0) {
        fprintf(stderr, "%s: completion error %d\n",
                operation, c.ret);
        exit(EXIT_FAILURE);
    }

    printf("%s completion OK\n", operation);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <vhost-user-socket>\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct blkio *b = NULL;

    check(blkio_create("virtio-blk-vhost-user", &b),
          "blkio_create");

    check(blkio_set_str(b, "path", argv[1]), "set path");

    check(blkio_connect(b), "blkio_connect");

    check(blkio_start(b), "blkio_start");

    printf("RFL queue setup OK\n");

    struct blkio_mem_region region = {0};

    check(blkio_alloc_mem_region(
              b, &region, 2 * BLOCK_SIZE),
          "blkio_alloc_mem_region");

    check(blkio_map_mem_region(b, &region),
          "blkio_map_mem_region");

    uint8_t *write_buf = region.addr;
    uint8_t *read_buf = write_buf + BLOCK_SIZE;

    for (size_t i = 0; i < BLOCK_SIZE; i++) {
        write_buf[i] = (uint8_t)((i * 73 + 19) & 0xff);
    }

    memset(read_buf, 0, BLOCK_SIZE);

    struct blkioq *q = blkio_get_queue(b, 0);

    if (!q) {
        fprintf(stderr, "Failed to obtain queue 0\n");
        exit(EXIT_FAILURE);
    }

    printf("Submitting WRITE\n");
    blkioq_write(q, OFFSET, write_buf, BLOCK_SIZE, NULL, 0);
    wait_completion(q, "WRITE");

    printf("Submitting READ\n");
    blkioq_read(q, OFFSET, read_buf, BLOCK_SIZE, NULL, 0);
    wait_completion(q, "READ");

    if (memcmp(write_buf, read_buf, BLOCK_SIZE) != 0) {
        fprintf(stderr, "DATA MISMATCH!\n");
        exit(EXIT_FAILURE);
    }

    printf("DATA VERIFIED: %d bytes identical\n", BLOCK_SIZE);

    // Both requests have completed: no outstanding I/O.
    // The original libblkio destroy path handles registered memory.
    blkio_destroy(&b);

    printf("Test completed successfully\n");
    return EXIT_SUCCESS;
}
