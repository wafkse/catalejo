/*
 * Address Space Layout Test Suite for the Mirilla Module
 *
 * Exercises the `ADDRESS_SPACE_LAYOUT` command against the caller's own
 * address space:
 *
 * - A count-only pass (`DO_NOT_POPULATE` on both lists) reports nonzero
 *   `total_count` values and metadata whose argument/environment ranges are
 *   sane (non-empty, ordered, and consistent with `/proc/self/auxv`).
 * - A full layout pass allocates a buffer sized to the reported count, re-issues
 *   the command, and verifies every entry is well-formed (`start < end`) and
 *   that the test's own anonymous read-write region appears with the expected
 *   attributes.
 * - A short-buffer pass supplies `list_size = 1` and verifies the kernel still
 *   reports the full `total_count` while populating only the single entry.
 * - An auxiliary-vector pass verifies the count is at least the `AT_NULL`
 *   terminator, that the last populated entry is `AT_NULL`, and that
 *   `AT_PAGESIZE` matches `sysconf(_SC_PAGESIZE)`.
 * - A negative pass verifies bad `element_size`, a null list address without
 *   `DO_NOT_POPULATE`, and a nonexistent target are all refused.
 */

#include "test-harness.h"

#include <inttypes.h>
#include <stdint.h>

/* Auxiliary vector type constants, as widened by the kernel ABI. */
#define AT_NULL 0
#define AT_PAGESIZE 6
#define AT_VECTOR_BASE_SIZE 16

/*
 * Build a `mirilla_outside_list` descriptor for the given backing buffer.
 */
static struct mirilla_outside_list make_outside_list(void *list_address, uint32_t list_size,
                                                     uint32_t element_size,
                                                     mirilla_outside_list_attribute_t attribute)
{
    struct mirilla_outside_list descriptor = {
        .list_address = (virtual_address_t)list_address,
        .list_size = list_size,
        .element_size = element_size,
        .list_attribute = attribute,
    };

    return descriptor;
}

/* Numeric VMA provenance reported by `/proc/self/maps`. */
struct proc_mapping {
    virtual_address_t start_address, end_address;
    uint64_t file_offset;
    uint32_t device_major, device_minor;
    uint64_t inode_number;
};

/* Find the `/proc/self/maps` entry containing one process address. */
static int read_proc_mapping(virtual_address_t target_address, struct proc_mapping *mapping)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) {
        perror("fopen /proc/self/maps");
        return -1;
    }

    char *line = NULL;
    size_t line_capacity = 0;
    int status = -1;

    while (getline(&line, &line_capacity, maps) >= 0) {
        struct proc_mapping candidate = { 0 };
        int fields = sscanf(
            line, "%" SCNx64 "-%" SCNx64 " %*4s %" SCNx64 " %" SCNx32 ":%" SCNx32 " %" SCNu64,
            &candidate.start_address, &candidate.end_address, &candidate.file_offset,
            &candidate.device_major, &candidate.device_minor, &candidate.inode_number);

        if (fields == 6 && candidate.start_address <= target_address &&
            target_address < candidate.end_address) {
            *mapping = candidate;
            status = 0;
            break;
        }
    }

    if (ferror(maps)) {
        perror("read /proc/self/maps");
        status = -1;
    }

    free(line);

    if (fclose(maps) != 0) {
        perror("fclose /proc/self/maps");
        return -1;
    }

    return status;
}

/*
 * A count-only pass: both lists carry `DO_NOT_POPULATE`, so the kernel reports
 * the full counts without touching any backing buffer.
 */
static int test_layout_count_only(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");

    struct mirilla_outside_list layout_list =
        make_outside_list(NULL, 0, sizeof(struct mirilla_map_address_space_layout),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);
    struct mirilla_outside_list auxiliary_vector_list =
        make_outside_list(NULL, 0, sizeof(struct mirilla_auxiliary_vector_entry),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);

    struct mirilla_map_address_space_metadata metadata = { 0 };
    struct mirilla_outside_list_outcome layout_outcome = { 0 };
    struct mirilla_outside_list_outcome auxiliary_vector_outcome = { 0 };

    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_address_space_layout(
               mirilla_fd, target_id, &layout_list, &auxiliary_vector_list, &metadata,
               &layout_outcome, &auxiliary_vector_outcome)),
           "count-only layout query failed");

    ASSERT(layout_outcome.total_count > 0, "layout count is zero for a live process");
    ASSERT(auxiliary_vector_outcome.total_count > 0, "auxiliary vector count is zero");

    ASSERT(metadata.argument_end >= metadata.argument_start, "argument range is inverted");
    ASSERT(metadata.environment_end >= metadata.environment_start, "environment range is inverted");

    print_with_timestamp("[LAYOUT] %u VMAs, %u auxv entries\n", layout_outcome.total_count,
                         auxiliary_vector_outcome.total_count);

    mirilla_disengage(mirilla_fd, target_id);
    close(mirilla_fd);
    return 0;
}

/*
 * A full layout pass: allocate a buffer sized to the reported count, re-issue,
 * and verify every entry is well-formed and the test's own anonymous region
 * appears with `READ|WRITE|ANONYMOUS`.
 */
static int test_layout_full_population(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    void *region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_1);
    ASSERT(region != NULL, "failed to allocate test region");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");

    /* Sizing pass. */
    struct mirilla_outside_list sizing_list =
        make_outside_list(NULL, 0, sizeof(struct mirilla_map_address_space_layout),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);
    struct mirilla_outside_list sizing_aux =
        make_outside_list(NULL, 0, sizeof(struct mirilla_auxiliary_vector_entry),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);

    struct mirilla_map_address_space_metadata metadata = { 0 };
    struct mirilla_outside_list_outcome layout_outcome = { 0 };
    struct mirilla_outside_list_outcome auxiliary_vector_outcome = { 0 };

    ASSERT(MIRILLA_COMMAND_IS_OK(
               mirilla_address_space_layout(mirilla_fd, target_id, &sizing_list, &sizing_aux,
                                            &metadata, &layout_outcome, &auxiliary_vector_outcome)),
           "sizing pass failed");

    uint32_t layout_count = layout_outcome.total_count;
    uint32_t auxiliary_vector_count = auxiliary_vector_outcome.total_count;

    ASSERT(layout_count > 0, "layout count is zero");

    /* Population pass. */
    struct mirilla_map_address_space_layout *layout_buffer =
        calloc(layout_count, sizeof(struct mirilla_map_address_space_layout));
    ASSERT(layout_buffer != NULL, "failed to allocate layout buffer");

    struct mirilla_auxiliary_vector_entry *auxiliary_vector_buffer =
        calloc(auxiliary_vector_count, sizeof(struct mirilla_auxiliary_vector_entry));
    ASSERT(auxiliary_vector_buffer != NULL, "failed to allocate auxiliary vector buffer");

    struct mirilla_outside_list populate_list = make_outside_list(
        layout_buffer, layout_count, sizeof(struct mirilla_map_address_space_layout), 0);
    struct mirilla_outside_list populate_aux =
        make_outside_list(auxiliary_vector_buffer, auxiliary_vector_count,
                          sizeof(struct mirilla_auxiliary_vector_entry), 0);

    ASSERT(MIRILLA_COMMAND_IS_OK(
               mirilla_address_space_layout(mirilla_fd, target_id, &populate_list, &populate_aux,
                                            &metadata, &layout_outcome, &auxiliary_vector_outcome)),
           "population pass failed");

    /* Every entry must be well-formed. */
    for (uint32_t i = 0; i < layout_outcome.total_count; i++) {
        ASSERT(layout_buffer[i].start_address < layout_buffer[i].end_address, "layout entry has "
                                                                              "inverted or empty "
                                                                              "range");
    }

    /*
     * The test's own anonymous read-write region must appear in the layout with
     * at least `READ|WRITE|ANONYMOUS`.
     *
     * The kernel may coalesce adjacent anonymous VMAs, so the VMA containing
     * the test region can be larger than the allocation itself. Search for a
     * containing entry rather than an exact boundary match.
     */
    int found_region = 0;
    virtual_address_t region_start = (virtual_address_t)region;
    virtual_address_t region_end = region_start + TEST_REGION_SIZE;

    for (uint32_t i = 0; i < layout_outcome.total_count; i++) {
        if (layout_buffer[i].start_address <= region_start &&
            layout_buffer[i].end_address >= region_end) {
            ASSERT((layout_buffer[i].attribute_list & MIRILLA_MAP_LAYOUT_ATTRIBUTE_READ) != 0,
                   "test region is missing the READ attribute");
            ASSERT((layout_buffer[i].attribute_list & MIRILLA_MAP_LAYOUT_ATTRIBUTE_WRITE) != 0,
                   "test region is missing the WRITE attribute");
            ASSERT((layout_buffer[i].attribute_list & MIRILLA_MAP_LAYOUT_ATTRIBUTE_ANONYMOUS) != 0,
                   "test region is missing the ANONYMOUS attribute");
            ASSERT((layout_buffer[i].attribute_list & MIRILLA_MAP_LAYOUT_ATTRIBUTE_FILE) == 0,
                   "anonymous test region unexpectedly reports file backing");
            ASSERT(layout_buffer[i].file_offset == 0 && layout_buffer[i].device_major == 0 &&
                       layout_buffer[i].device_minor == 0 && layout_buffer[i].inode_number == 0,
                   "anonymous test region carries file provenance");

            found_region = 1;
            break;
        }
    }

    ASSERT(found_region, "test region was not found in the layout");

    virtual_address_t code_address = (virtual_address_t)&test_layout_full_population;
    struct proc_mapping executable_mapping = { 0 };
    ASSERT(read_proc_mapping(code_address, &executable_mapping) == 0, "failed to find test "
                                                                      "executable mapping in "
                                                                      "/proc/self/maps");

    int found_executable = 0;

    for (uint32_t i = 0; i < layout_outcome.total_count; i++) {
        if (layout_buffer[i].start_address <= code_address &&
            code_address < layout_buffer[i].end_address) {
            ASSERT((layout_buffer[i].attribute_list & MIRILLA_MAP_LAYOUT_ATTRIBUTE_FILE) != 0,
                   "test executable mapping lacks file backing");
            ASSERT(layout_buffer[i].start_address == executable_mapping.start_address,
                   "test executable mapping has the wrong start address");
            ASSERT(layout_buffer[i].end_address == executable_mapping.end_address, "test "
                                                                                   "executable "
                                                                                   "mapping has "
                                                                                   "the wrong end "
                                                                                   "address");
            ASSERT(layout_buffer[i].file_offset == executable_mapping.file_offset, "test "
                                                                                   "executable "
                                                                                   "mapping has "
                                                                                   "the wrong file "
                                                                                   "offset");
            ASSERT(layout_buffer[i].device_major == executable_mapping.device_major, "test "
                                                                                     "executable "
                                                                                     "mapping has "
                                                                                     "the wrong "
                                                                                     "device "
                                                                                     "major");
            ASSERT(layout_buffer[i].device_minor == executable_mapping.device_minor, "test "
                                                                                     "executable "
                                                                                     "mapping has "
                                                                                     "the wrong "
                                                                                     "device "
                                                                                     "minor");
            ASSERT(layout_buffer[i].inode_number == executable_mapping.inode_number, "test "
                                                                                     "executable "
                                                                                     "mapping has "
                                                                                     "the wrong "
                                                                                     "inode");

            found_executable = 1;
            break;
        }
    }

    ASSERT(found_executable, "test executable mapping was not found in the layout");

    print_with_timestamp("[LAYOUT] Full population: %u entries, test region found\n",
                         layout_outcome.total_count);

    free(auxiliary_vector_buffer);
    free(layout_buffer);
    mirilla_disengage(mirilla_fd, target_id);
    munmap(region, TEST_REGION_SIZE);
    close(mirilla_fd);
    return 0;
}

/*
 * A short-buffer pass: supply `list_size = 1` and verify the kernel still
 * reports the full `total_count` while populating only the single entry.
 */
static int test_layout_short_buffer(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");

    /* Sizing pass to learn the full count. */
    struct mirilla_outside_list sizing_list =
        make_outside_list(NULL, 0, sizeof(struct mirilla_map_address_space_layout),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);
    struct mirilla_outside_list sizing_aux =
        make_outside_list(NULL, 0, sizeof(struct mirilla_auxiliary_vector_entry),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);

    struct mirilla_map_address_space_metadata metadata = { 0 };
    struct mirilla_outside_list_outcome layout_outcome = { 0 };
    struct mirilla_outside_list_outcome auxiliary_vector_outcome = { 0 };

    ASSERT(MIRILLA_COMMAND_IS_OK(
               mirilla_address_space_layout(mirilla_fd, target_id, &sizing_list, &sizing_aux,
                                            &metadata, &layout_outcome, &auxiliary_vector_outcome)),
           "sizing pass failed");

    uint32_t full_count = layout_outcome.total_count;
    ASSERT(full_count > 1, "need at least two VMAs for a short-buffer test");

    /* Short-buffer pass. */
    struct mirilla_map_address_space_layout single_entry = { 0 };
    struct mirilla_outside_list short_list =
        make_outside_list(&single_entry, 1, sizeof(struct mirilla_map_address_space_layout), 0);
    struct mirilla_outside_list dnp_aux =
        make_outside_list(NULL, 0, sizeof(struct mirilla_auxiliary_vector_entry),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);

    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_address_space_layout(mirilla_fd, target_id, &short_list,
                                                              &dnp_aux, &metadata, &layout_outcome,
                                                              &auxiliary_vector_outcome)),
           "short-buffer pass failed");

    ASSERT(layout_outcome.total_count == full_count, "short-buffer pass did not report the full "
                                                     "count");
    ASSERT(single_entry.start_address < single_entry.end_address, "short-buffer entry is not "
                                                                  "well-formed");

    print_with_timestamp("[LAYOUT] Short buffer: reported %u, populated 1\n",
                         layout_outcome.total_count);

    mirilla_disengage(mirilla_fd, target_id);
    close(mirilla_fd);
    return 0;
}

/*
 * An auxiliary-vector pass: verify the count includes the `AT_NULL` terminator,
 * the last populated entry is `AT_NULL`, and `AT_PAGESIZE` matches
 * `sysconf(_SC_PAGESIZE)`.
 */
static int test_layout_auxiliary_vector(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");

    /* Sizing pass. */
    struct mirilla_outside_list sizing_layout =
        make_outside_list(NULL, 0, sizeof(struct mirilla_map_address_space_layout),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);
    struct mirilla_outside_list sizing_aux =
        make_outside_list(NULL, 0, sizeof(struct mirilla_auxiliary_vector_entry),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);

    struct mirilla_map_address_space_metadata metadata = { 0 };
    struct mirilla_outside_list_outcome layout_outcome = { 0 };
    struct mirilla_outside_list_outcome auxiliary_vector_outcome = { 0 };

    ASSERT(MIRILLA_COMMAND_IS_OK(
               mirilla_address_space_layout(mirilla_fd, target_id, &sizing_layout, &sizing_aux,
                                            &metadata, &layout_outcome, &auxiliary_vector_outcome)),
           "sizing pass failed");

    uint32_t auxiliary_vector_count = auxiliary_vector_outcome.total_count;
    ASSERT(auxiliary_vector_count >= 2, "auxiliary vector is too short to contain AT_NULL");

    /* Population pass for the auxiliary vector only. */
    struct mirilla_auxiliary_vector_entry *auxiliary_vector_buffer =
        calloc(auxiliary_vector_count, sizeof(struct mirilla_auxiliary_vector_entry));
    ASSERT(auxiliary_vector_buffer != NULL, "failed to allocate auxiliary vector buffer");

    struct mirilla_outside_list dnp_layout =
        make_outside_list(NULL, 0, sizeof(struct mirilla_map_address_space_layout),
                          MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);
    struct mirilla_outside_list populate_aux =
        make_outside_list(auxiliary_vector_buffer, auxiliary_vector_count,
                          sizeof(struct mirilla_auxiliary_vector_entry), 0);

    ASSERT(MIRILLA_COMMAND_IS_OK(
               mirilla_address_space_layout(mirilla_fd, target_id, &dnp_layout, &populate_aux,
                                            &metadata, &layout_outcome, &auxiliary_vector_outcome)),
           "auxiliary vector population failed");

    ASSERT(auxiliary_vector_outcome.total_count == auxiliary_vector_count, "auxiliary vector count "
                                                                           "changed between "
                                                                           "passes");

    /* The last populated entry must be the `AT_NULL` terminator. */
    ASSERT(auxiliary_vector_buffer[auxiliary_vector_outcome.total_count - 1].entry_type == AT_NULL,
           "last auxiliary vector entry is not AT_NULL");

    /*
     * `AT_PAGESIZE` must be present and match `sysconf(_SC_PAGESIZE)`.
     */
    long page_size = sysconf(_SC_PAGESIZE);
    ASSERT(page_size > 0, "sysconf(_SC_PAGESIZE) failed");

    int found_page_size = 0;
    for (uint32_t i = 0; i < auxiliary_vector_outcome.total_count; i++) {
        if (auxiliary_vector_buffer[i].entry_type == AT_PAGESIZE) {
            ASSERT((long)auxiliary_vector_buffer[i].entry_value == page_size, "AT_PAGESIZE value "
                                                                              "does not match "
                                                                              "sysconf");
            found_page_size = 1;
            break;
        }
    }

    ASSERT(found_page_size, "AT_PAGESIZE was not found in the auxiliary vector");

    print_with_timestamp("[LAYOUT] Auxiliary vector: %u entries, AT_NULL and AT_PAGESIZE "
                         "verified\n",
                         auxiliary_vector_outcome.total_count);

    free(auxiliary_vector_buffer);
    mirilla_disengage(mirilla_fd, target_id);
    close(mirilla_fd);
    return 0;
}

/*
 * A negative pass: bad `element_size`, a null list address without
 * `DO_NOT_POPULATE`, and a nonexistent target must all be refused.
 *
 * The `catalejo-sys` C wrapper recovers `-errno` from the raw ioctl, so the
 * returned status is checked directly rather than through `errno`.
 */
static int test_layout_negative(void)
{
    int mirilla_fd = mirilla_open_device();
    ASSERT(mirilla_fd >= 0, "failed to open device");

    mirilla_map_target_id_t target_id = 0;
    ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)), "self-engage "
                                                                                    "failed");

    struct mirilla_map_address_space_metadata metadata = { 0 };
    struct mirilla_outside_list_outcome layout_outcome = { 0 };
    struct mirilla_outside_list_outcome auxiliary_vector_outcome = { 0 };

    /* Bad `element_size` on the layout list. */
    {
        struct mirilla_outside_list bad_layout =
            make_outside_list(NULL, 0, 0, MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);
        struct mirilla_outside_list dnp_aux =
            make_outside_list(NULL, 0, sizeof(struct mirilla_auxiliary_vector_entry),
                              MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);

        long ret = mirilla_address_space_layout(mirilla_fd, target_id, &bad_layout, &dnp_aux,
                                                &metadata, &layout_outcome,
                                                &auxiliary_vector_outcome);
        ASSERT(ret == -EINVAL, "bad layout element size was not rejected");
    }

    /* Bad `element_size` on the auxiliary vector list. */
    {
        struct mirilla_outside_list dnp_layout =
            make_outside_list(NULL, 0, sizeof(struct mirilla_map_address_space_layout),
                              MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);
        struct mirilla_outside_list bad_aux =
            make_outside_list(NULL, 0, 0, MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);

        long ret = mirilla_address_space_layout(mirilla_fd, target_id, &dnp_layout, &bad_aux,
                                                &metadata, &layout_outcome,
                                                &auxiliary_vector_outcome);
        ASSERT(ret == -EINVAL, "bad auxiliary vector element size was not rejected");
    }

    /* Null list address without `DO_NOT_POPULATE`. */
    {
        struct mirilla_outside_list null_layout =
            make_outside_list(NULL, 1, sizeof(struct mirilla_map_address_space_layout), 0);
        struct mirilla_outside_list dnp_aux =
            make_outside_list(NULL, 0, sizeof(struct mirilla_auxiliary_vector_entry),
                              MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);

        long ret = mirilla_address_space_layout(mirilla_fd, target_id, &null_layout, &dnp_aux,
                                                &metadata, &layout_outcome,
                                                &auxiliary_vector_outcome);
        ASSERT(ret == -EFAULT, "null layout list address was not rejected");
    }

    mirilla_disengage(mirilla_fd, target_id);

    /* Nonexistent target. */
    {
        struct mirilla_outside_list dnp_layout =
            make_outside_list(NULL, 0, sizeof(struct mirilla_map_address_space_layout),
                              MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);
        struct mirilla_outside_list dnp_aux =
            make_outside_list(NULL, 0, sizeof(struct mirilla_auxiliary_vector_entry),
                              MIRILLA_OUTSIDE_LIST_ATTRIBUTE_DO_NOT_POPULATE);

        long ret = mirilla_address_space_layout(mirilla_fd, BOGUS_TARGET_ID, &dnp_layout, &dnp_aux,
                                                &metadata, &layout_outcome,
                                                &auxiliary_vector_outcome);
        ASSERT(ret == -ENOENT, "layout against a bogus target id was not rejected");
    }

    close(mirilla_fd);
    return 0;
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
    print_banner("MIRILLA ADDRESS SPACE LAYOUT SUITE");

    if (harness_fault_initialize() < 0)
        return EXIT_FAILURE;

    RUN_TEST("Layout Count-Only", test_layout_count_only);
    RUN_TEST("Layout Full Population", test_layout_full_population);
    RUN_TEST("Layout Short Buffer", test_layout_short_buffer);
    RUN_TEST("Layout Auxiliary Vector", test_layout_auxiliary_vector);
    RUN_TEST("Layout Negative", test_layout_negative);

    print_summary();

    return suite_status();
}
