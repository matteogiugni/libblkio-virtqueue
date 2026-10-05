// SPDX-License-Identifier: (MIT OR Apache-2.0)
#include "util.h"

enum {
    TEST_FILE_SIZE = 2 * 1024 * 1024,
};

static char filename[] = "zone-mgmt-XXXXXX";

#define ZONE_SIZE 67108864
#define NR_ZONES 2

enum zone_state {
   EMPTY = BLKIO_ZONE_STATE_EMPTY,
   IMP_OPEN = BLKIO_ZONE_STATE_IMP_OPEN,
   EXP_OPEN = BLKIO_ZONE_STATE_EXP_OPEN,
   CLOSED = BLKIO_ZONE_STATE_CLOSED,
   FULL = BLKIO_ZONE_STATE_FULL,
};

struct result {
   enum zone_state state;
   bool ret;
};

struct result one_zone_state[4][4] = {{{CLOSED, true}, {FULL, true}, {EXP_OPEN, true}, {EMPTY, true}},
                                        {{FULL, false}, {FULL, true}, {FULL, false}, {EMPTY, true}},
                                        {{CLOSED, true}, {FULL, true}, {EXP_OPEN, true}, {EMPTY, true}},
                                        {{EMPTY, false}, {FULL, true}, {EXP_OPEN, true}, {EMPTY, true}}};

enum zone_state all_zones_state[4][4] = {{CLOSED, FULL, EXP_OPEN, EMPTY},
                                         {FULL, FULL, FULL, EMPTY},
                                         {CLOSED, FULL, EXP_OPEN, EMPTY},
                                         {EMPTY, EMPTY, EMPTY, EMPTY}};

enum zone_state from_IMP_OPEN[4] = {CLOSED, FULL, EXP_OPEN, EMPTY};
struct result to_IMP_OPEN[4] = {{IMP_OPEN, true}, {FULL, false}, {EXP_OPEN, true}, {IMP_OPEN, true}};

enum zone_state from_all_IMP_OPEN[4] = {CLOSED, FULL, IMP_OPEN, EMPTY};

static void back_to_exp_open(struct blkioq *q, struct blkio_completion *completion,
     struct blkio_zone *zones, bool all_zones) {
    if (all_zones) {
        blkioq_reset_all_zones(q, NULL, 0);
    } else {
        blkioq_reset_zone(q, 0, NULL, 0);
    }
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret == 0);

    int nr_zones = all_zones ? NR_ZONES : 1;
    for (int i = 0; i < nr_zones; i++) {
        blkioq_open_zone(q, i * ZONE_SIZE, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert(completion->ret == 0);
    }

    blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret > 0);
    for (int i = 0; i < nr_zones; i++) {
        assert(zones[i].zone_state == BLKIO_ZONE_STATE_EXP_OPEN);
    }
}

static void back_to_imp_open(struct blkioq *q, struct blkio_completion *completion,
        struct blkio_zone *zones, void *buf, size_t buf_size, bool all_zones) {
    if (all_zones) {
        blkioq_reset_all_zones(q, NULL, 0);
    } else {
        blkioq_reset_zone(q, 0, NULL, 0);
    }
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret == 0);

    int nr_zones = all_zones ? NR_ZONES : 1;
    for (int i = 0; i < nr_zones; i++) {
        blkioq_write(q, i * ZONE_SIZE, buf, buf_size, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert(completion->ret == 0);
    }

    blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret > 0);
    for (int i = 0; i < nr_zones; i++) {
        assert(zones[i].zone_state == BLKIO_ZONE_STATE_IMP_OPEN);
    }
}

static void back_to_closed(struct blkioq *q, struct blkio_completion *completion,
        struct blkio_zone *zones, bool all_zones) {
    if (all_zones) {
        blkioq_reset_all_zones(q, NULL, 0);
    } else {
        blkioq_reset_zone(q, 0, NULL, 0);
    }
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret == 0);

    int nr_zones = all_zones ? NR_ZONES : 1;
    for (int i = 0; i < nr_zones; i++) {
        blkioq_open_zone(q, i * ZONE_SIZE, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert(completion->ret == 0);
    }
    if (all_zones) {
        blkioq_close_all_zones(q, NULL, 0);
    } else {
        blkioq_close_zone(q, 0, NULL, 0);
    }
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret == 0);

    blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret > 0);
    for (int i = 0; i < nr_zones; i++) {
        assert(zones[i].zone_state == BLKIO_ZONE_STATE_CLOSED);
    }
}

static void back_to_full(struct blkioq *q, struct blkio_completion *completion,
        struct blkio_zone *zones, bool all_zones) {
    if (all_zones) {
        blkioq_reset_all_zones(q, NULL, 0);
    } else {
        blkioq_reset_zone(q, 0, NULL, 0);
    }
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret == 0);

    int nr_zones1 = all_zones ? NR_ZONES : 0;
    for (int i = 0; i < nr_zones1; i++) {
        blkioq_open_zone(q, i * ZONE_SIZE, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert(completion->ret == 0);
    }
    if (all_zones) {
        blkioq_finish_all_zones(q, NULL, 0);
    } else {
        blkioq_finish_zone(q, 0, NULL, 0);
    }
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret == 0);

    int nr_zones2 = all_zones ? NR_ZONES : 1;
    blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret > 0);
    for (int i = 0; i < nr_zones2; i++) {
        assert(zones[i].zone_state == BLKIO_ZONE_STATE_FULL);
    }
}

static void back_to_empty(struct blkioq *q, struct blkio_completion *completion,
        struct blkio_zone *zones, bool all_zones) {
    if (all_zones) {
        blkioq_reset_all_zones(q, NULL, 0);
    } else {
        blkioq_reset_zone(q, 0, NULL, 0);
    }
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret == 0);

    int nr_zones = all_zones ? NR_ZONES : 1;
    blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret > 0);
    for (int i = 0; i < nr_zones; i++) {
        assert(zones[i].zone_state == BLKIO_ZONE_STATE_EMPTY);
    }
}

static void test_from_imp_open(struct blkioq *q, struct blkio_completion *completion,
    struct blkio_zone *zones, void *buf, size_t buf_size,
    void (*mgmt_state[4])(struct blkioq *, uint64_t, void *, uint32_t)) {
    for (int i = 0; i < 4; i++) {
        back_to_imp_open(q, completion, zones, buf, buf_size, false);
        mgmt_state[i](q, 0, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert(completion->ret == 0);

        blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert(completion->ret > 0);
        assert(zones[0].zone_state == from_IMP_OPEN[i]);
        assert(zones[1].zone_state == EMPTY);
    }
    back_to_imp_open(q, completion, zones, buf, buf_size, false);
    blkioq_write(q, buf_size, buf, buf_size, NULL, 0);
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret == 0);

    blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
    assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
    assert(completion->ret > 0);
    assert(zones[0].zone_state == IMP_OPEN);
    assert(zones[1].zone_state == EMPTY);
}

static void test_to_imp_open(struct blkioq *q, struct blkio_completion *completion,
    struct blkio_zone *zones, void *buf, size_t buf_size,
    void (*back_to_state[4])(struct blkioq *, struct blkio_completion *, struct blkio_zone *, bool)) {
    for (int i = 0; i < 4; i++) {
        back_to_state[i](q, completion, zones, false);
        blkioq_write(q, 0, buf, buf_size, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert((completion->ret == 0) == to_IMP_OPEN[i].ret);

        blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert(completion->ret > 0);
        assert(zones[0].zone_state == to_IMP_OPEN[i].state);
        assert(zones[1].zone_state == EMPTY);
    }
}

static void test_from_imp_open_all(struct blkioq *q, struct blkio_completion *completion,
    struct blkio_zone *zones, void *buf, size_t buf_size,
    void (*mgmt_all_zones[4])(struct blkioq *, void *, uint32_t)) {
    for (int i = 0; i < 4; i++) {
        back_to_imp_open(q, completion, zones, buf, buf_size, true);
        mgmt_all_zones[i](q, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert(completion->ret == 0);

        blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
        assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
        assert(completion->ret > 0);
        assert(zones[0].zone_state == from_all_IMP_OPEN[i]);
        assert(zones[1].zone_state == from_all_IMP_OPEN[i]);
    }
}

static void test_states(struct blkioq *q, struct blkio_completion *completion,
    struct blkio_zone *zones,
    void (*back_to_state[4])(struct blkioq *, struct blkio_completion *, struct blkio_zone *, bool),
    void (*mgmt_zone[4])(struct blkioq *, uint64_t, void *, uint32_t)){
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            back_to_state[i](q, completion, zones, false);
            mgmt_zone[j](q, 0, NULL, 0);
            assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
            assert((completion->ret == 0) == one_zone_state[i][j].ret);

            blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
            assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
            assert(completion->ret > 0);
            assert(zones[0].zone_state == one_zone_state[i][j].state);
            assert(zones[1].zone_state == EMPTY);
        }
    }
}

static void test_states_all(struct blkioq *q, struct blkio_completion *completion,
    struct blkio_zone *zones,
    void (*back_to_state[4])(struct blkioq *, struct blkio_completion *, struct blkio_zone *, bool),
    void (*mgmt_all_zones[4])(struct blkioq *, void *, uint32_t)){
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            back_to_state[i](q, completion, zones, true);
            mgmt_all_zones[j](q, NULL, 0);
            assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
            assert(completion->ret == 0);

            blkioq_report_zones(q, 0, zones, NR_ZONES, NULL, 0);
            assert(blkioq_do_io(q, completion, 1, 1, NULL) == 1);
            assert(completion->ret > 0);
            assert(zones[0].zone_state == all_zones_state[i][j]);
            assert(zones[1].zone_state == all_zones_state[i][j]);
        }
    }
}

int main(int argc, char **argv)
{
    struct test_opts opts;
    struct blkio *b;
    struct blkioq *q;
    struct blkio_completion completion;
    struct blkio_zone zones[NR_ZONES];

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

    void (*back_to_state[4])(struct blkioq *, struct blkio_completion *, struct blkio_zone *, bool) =
        {back_to_closed, back_to_full, back_to_exp_open, back_to_empty};

    void (*mgmt_zone[4])(struct blkioq *, uint64_t, void *, uint32_t) =
        {blkioq_close_zone, blkioq_finish_zone, blkioq_open_zone, blkioq_reset_zone};

    void (*mgmt_all_zones[4])(struct blkioq *, void *, uint32_t) =
        {blkioq_close_all_zones, blkioq_finish_all_zones, blkioq_open_all_zones, blkioq_reset_all_zones};

    test_states(q, &completion, zones, back_to_state, mgmt_zone);
    test_states_all(q, &completion, zones, back_to_state, mgmt_all_zones);

    test_from_imp_open(q, &completion, zones, buf, buf_size, mgmt_zone);
    test_to_imp_open(q, &completion, zones, buf, buf_size, back_to_state);
    test_from_imp_open_all(q, &completion, zones, buf, buf_size, mgmt_all_zones);

    blkio_destroy(&b);
    return 0;
}
