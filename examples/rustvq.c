
#include <blkio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>

#define BLOCK_SIZE 4096
#define OFFSET (1024 * 1024)
#define MAX_INFLIGHT 64
#define STRESS_ROUNDS 128
#define BATCH_OFFSET (4 * 1024 * 1024)

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


struct request_tag {
    unsigned index;
};

static void wait_batch(
    struct blkioq *q,
    struct request_tag *tags,
    unsigned depth,
    const char *phase,
    unsigned round)
{
    bool seen[MAX_INFLIGHT] = {false};
    unsigned completed = 0;

    while (completed < depth) {
        struct blkio_completion completions[MAX_INFLIGHT] = {0};
        struct timespec timeout = {
            .tv_sec = 5,
            .tv_nsec = 0,
        };

        int ret = blkioq_do_io(
            q,
            completions,
            1,
            (int)(depth - completed),
            &timeout
        );

        if (ret <= 0) {
            fprintf(stderr,
                    "%s round %u: do_io failed: %d (%s)\n",
                    phase, round, ret, blkio_get_error_msg());
            exit(EXIT_FAILURE);
        }

        for (int k = 0; k < ret; k++) {
            struct blkio_completion *c = &completions[k];
            int id = -1;

            // Compare addresses without dereferencing user_data.
            for (unsigned i = 0; i < depth; i++) {
                if (c->user_data == &tags[i]) {
                    id = (int)i;
                    break;
                }
            }

            if (id < 0) {
                fprintf(stderr,
                        "%s round %u: unknown user_data\n",
                        phase, round);
                exit(EXIT_FAILURE);
            }

            if (seen[id]) {
                fprintf(stderr,
                        "%s round %u: duplicate completion %d\n",
                        phase, round, id);
                exit(EXIT_FAILURE);
            }

            if (c->ret != 0) {
                fprintf(stderr,
                        "%s round %u: request %d failed: %d (%s)\n",
                        phase, round, id, c->ret,
                        c->error_msg ? c->error_msg : "");
                exit(EXIT_FAILURE);
            }

            seen[id] = true;
            completed++;
        }
    }

    for (unsigned i = 0; i < depth; i++) {
        if (!seen[i]) {
            fprintf(stderr,
                    "%s round %u: missing completion %u\n",
                    phase, round, i);
            exit(EXIT_FAILURE);
        }
    }
}

static void run_batch(
    struct blkioq *q,
    uint8_t *write_buf,
    uint8_t *read_buf,
    unsigned depth,
    unsigned round)
{
    struct request_tag tags[MAX_INFLIGHT];

    if (depth == 0 || depth > MAX_INFLIGHT) {
        fprintf(stderr, "Invalid test depth\n");
        exit(EXIT_FAILURE);
    }

    // Prepare distinct data for every request and round.
    for (unsigned i = 0; i < depth; i++) {
        tags[i].index = i;

        uint8_t *w = write_buf + i * BLOCK_SIZE;
        uint8_t *r = read_buf + i * BLOCK_SIZE;

        for (unsigned j = 0; j < BLOCK_SIZE; j++) {
            w[j] = (uint8_t)(
                (round * 17U + i * 47U + j * 13U) & 0xff
            );
        }

        memset(r, 0, BLOCK_SIZE);
    }

    // Submit ALL writes before collecting any completion.
    for (unsigned i = 0; i < depth; i++) {
        blkioq_write(
            q,
            BATCH_OFFSET + (uint64_t)i * BLOCK_SIZE,
            write_buf + i * BLOCK_SIZE,
            BLOCK_SIZE,
            &tags[i],
            0
        );
    }

    wait_batch(q, tags, depth, "WRITE", round);

    // All writes have completed before we start reading.
    for (unsigned i = 0; i < depth; i++) {
        blkioq_read(
            q,
            BATCH_OFFSET + (uint64_t)i * BLOCK_SIZE,
            read_buf + i * BLOCK_SIZE,
            BLOCK_SIZE,
            &tags[i],
            0
        );
    }

    wait_batch(q, tags, depth, "READ", round);

    // Check every block independently.
    for (unsigned i = 0; i < depth; i++) {
        uint8_t *w = write_buf + i * BLOCK_SIZE;
        uint8_t *r = read_buf + i * BLOCK_SIZE;

        if (memcmp(w, r, BLOCK_SIZE) != 0) {
            fprintf(stderr,
                    "DATA MISMATCH: round=%u block=%u\n",
                    round, i);
            exit(EXIT_FAILURE);
        }
    }
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
            b, &region, 2 * MAX_INFLIGHT * BLOCK_SIZE),
        "blkio_alloc_mem_region");

    check(blkio_map_mem_region(b, &region),
          "blkio_map_mem_region");

    uint8_t *write_buf = region.addr;
    uint8_t *read_buf = write_buf + MAX_INFLIGHT * BLOCK_SIZE;

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

    printf("Testing two outstanding requests\n");

    run_batch(q, write_buf, read_buf, 2, 0);

    printf("Two outstanding requests: PASSED\n");

    printf(
        "Testing ring recycling: %d rounds, depth=%d\n",
        STRESS_ROUNDS,
        MAX_INFLIGHT
    );

    for (unsigned round = 0; round < STRESS_ROUNDS; round++) {
        run_batch(
            q,
            write_buf,
            read_buf,
            MAX_INFLIGHT,
            round
        );
    }

    printf("Ring recycling stress test: PASSED\n");

    printf("Testing %d outstanding requests\n", MAX_INFLIGHT);

    run_batch(
        q,
        write_buf,
        read_buf,
        MAX_INFLIGHT,
        STRESS_ROUNDS
    );

    printf("%d outstanding requests: PASSED\n", MAX_INFLIGHT);

    printf("Testing scatter-gather WRITEV/READV\n");

    struct iovec wiov[4];
    struct iovec riov[4];

    for (unsigned i = 0; i < BLOCK_SIZE; i++) {
        write_buf[i] = (uint8_t)((i * 31U + 7U) & 0xff);
    }

    memset(read_buf, 0, BLOCK_SIZE);

    for (unsigned i = 0; i < 4; i++) {
        wiov[i].iov_base = write_buf + i * 1024;
        wiov[i].iov_len = 1024;

        riov[i].iov_base = read_buf + i * 1024;
        riov[i].iov_len = 1024;
    }

    blkioq_writev(
        q, 8 * 1024 * 1024,
        wiov, 4, NULL, 0
    );

    wait_completion(q, "WRITEV");

    blkioq_readv(
        q, 8 * 1024 * 1024,
        riov, 4, NULL, 0
    );

    wait_completion(q, "READV");

    if (memcmp(write_buf, read_buf, BLOCK_SIZE) != 0) {
        fprintf(stderr, "Scatter-gather data mismatch\n");
        exit(EXIT_FAILURE);
    }

    printf("Scatter-gather WRITEV/READV: PASSED\n");

    fprintf(stderr, "C TEST: before blkio_destroy\n");
    // Both requests have completed: no outstanding I/O.
    // The original libblkio destroy path handles registered memory.
    blkio_destroy(&b);
    fprintf(stderr, "C TEST: after blkio_destroy\n");

    printf("Test completed successfully\n");
    return EXIT_SUCCESS;
}
