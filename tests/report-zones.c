// SPDX-License-Identifier: (MIT OR Apache-2.0)
#include "util.h"

enum {
    TEST_FILE_SIZE = 2 * 1024 * 1024,
};

static char filename[] = "report-zones-XXXXXX";

void report_check(struct blkio_zone *zones, int nr_zones, uint64_t zone_size, size_t bytes_written) {
    for (size_t i = 0; i < nr_zones; i++) {
        uint64_t device_zone_number = zones[i].start / zone_size;
        bool is_written = zones[i].write_pointer > zones[i].start;

        assert(zones[i].start == device_zone_number * zone_size);
        assert(zones[i].capacity == zone_size);

        assert(zones[i].zone_type == BLKIO_ZONE_TYPE_SEQWRITE_REQ);
        assert(zones[i].reset == 0); // reset zone is not recommended
        if (is_written) {
            assert(zones[i].zone_state == BLKIO_ZONE_STATE_IMP_OPEN);
            assert(zones[i].write_pointer == bytes_written + device_zone_number * zone_size);
        } else {
            assert(zones[i].zone_state == BLKIO_ZONE_STATE_EMPTY);
            assert(zones[i].write_pointer == device_zone_number * zone_size);
        }
    }
}

int main(int argc, char **argv)
{
    struct test_opts opts;

    struct blkio *b;
    struct blkioq *q;
    struct blkio_completion completion;

    uint32_t nr_zones = 8;
    struct blkio_zone zones[nr_zones];
    uint64_t nr_zones_expected;
    uint64_t zone_size;
    uint64_t offset = 0;

    struct blkio_mem_region mem_region;
    void *buf;     /* I/O buffer */
    void *pattern;
    size_t buf_size;

    parse_generic_opts(&opts, argc, argv);

    buf_size = sysconf(_SC_PAGESIZE);

    /* Initialize pattern buffer */
    pattern = malloc(buf_size);
    assert(pattern);
    memset(pattern, 'A', buf_size);

    create_and_connect(&b, &opts, filename, TEST_FILE_SIZE);

    /* Set up I/O buffer */
    ok(blkio_alloc_mem_region(b, &mem_region, buf_size));
    buf = mem_region.addr;
    memcpy(buf, pattern, buf_size);

    ok(blkio_set_int(b, "num-queues", 1));
    ok(blkio_start(b));

    ok(blkio_map_mem_region(b, &mem_region));

    q = blkio_get_queue(b, 0);
    assert(q);

    blkio_get_uint64(b, "nr-zones", &nr_zones_expected);
    blkio_get_uint64(b, "zone-size", &zone_size);

    // Report zones for empty device
    blkioq_report_zones(q, offset, zones, nr_zones, NULL, 0);
    assert(blkioq_do_io(q, &completion, 1, 1, NULL) == 1);
    assert(completion.ret == nr_zones_expected - offset / zone_size);
    report_check(zones, completion.ret, zone_size, 0);

    blkioq_write(q, 0x0, buf, buf_size, NULL, 0);
    assert(blkioq_do_io(q, &completion, 1, 1, NULL) == 1);
    assert(completion.ret == 0);

    // Report zones for one zone written
    blkioq_report_zones(q, offset, zones, nr_zones, NULL, 0);
    assert(blkioq_do_io(q, &completion, 1, 1, NULL) == 1);
    assert(completion.ret == nr_zones_expected - offset / zone_size);
    report_check(zones, completion.ret, zone_size, buf_size);

    blkio_destroy(&b);
    return 0;
}
