/*
 * Self-Observation Test Suite for the Mirilla Module
 *
 * A process may engage its own thread group without `CAP_SYS_PTRACE` and
 * open peepholes over its own address space. These cases exercise the
 * self-peephole fault path (the target address space is the faulting
 * task's own `mm`, so the fault handler reuses the already-held
 * `mmap_lock`) and the `MAP_FIXED` placement of the peephole view,
 * including the degenerate placement over the observed range itself.
 *
 * Expected semantics, per the fault and notifier paths in
 * `src/mirilla-map.c`:
 * - A peephole only goes dead on `MMU_NOTIFY_RELEASE` (target teardown).
 * - Unmapping or clobbering the observed range keeps the peephole alive;
 *   the next fault re-resolves the target address, so fresh anonymous
 *   memory reads back its new contents, while an unresolvable address
 *   (or one covered by a `VM_IO` mapping) faults.
 *
 * All view accesses ride the `catalejo-fault` protected routines, so a
 * fault the module raises surfaces as `CATALEJO_OUTCOME_ERROR` end to end.
 */

#include "test-harness.h"

/*
 * Engage the caller's own thread group and disengage again.
 */
static int test_self_engage(void)
{
	int mirilla_fd = mirilla_open_device();
	ASSERT(mirilla_fd >= 0, "failed to open device");

	mirilla_map_target_id_t target_id = 0;

	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)),
	       "self-engage failed");
	ASSERT(target_id != 0, "self-engage returned a zero target id");

	print_with_timestamp("[SELF] Engaged self (pid %d), target_id = %lu\n", getpid(),
			     (unsigned long)target_id);

	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_disengage(mirilla_fd, target_id)),
	       "self-disengage failed");

	close(mirilla_fd);
	return 0;
}

/*
 * Open a peephole over the caller's own memory and read it back through a
 * kernel-chosen view address.
 */
static int test_self_peephole_basic(void)
{
	int mirilla_fd = mirilla_open_device();
	ASSERT(mirilla_fd >= 0, "failed to open device");

	void *region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_1);
	ASSERT(region != NULL, "failed to allocate test region");

	mirilla_map_target_id_t target_id = 0;
	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)),
	       "self-engage failed");

	mirilla_map_peephole_id_t peephole_id = 0;
	int peephole_fd = -1;
	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_peephole(
		       mirilla_fd, target_id, (virtual_address_t)region,
		       (virtual_address_t)region + TEST_REGION_SIZE, &peephole_id, &peephole_fd)),
	       "self-peephole failed");

	void *view = mmap(NULL, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE, peephole_fd, 0);
	ASSERT(view != MAP_FAILED, "failed to mmap peephole view");

	print_with_timestamp("[SELF] Region at %p mirrored at %p\n", region, view);

	/*
	 * The read below faults through the self-peephole path: the target
	 * address space is our own `mm`, whose `mmap_lock` the fault already
	 * holds.
	 */
	ASSERT(verify_region_faultable(view, TEST_REGION_SIZE, MAGIC_VALUE_1) == 0,
	       "view does not mirror the observed region");

	munmap(view, TEST_REGION_SIZE);
	close(peephole_fd);
	munmap(region, TEST_REGION_SIZE);
	mirilla_disengage(mirilla_fd, target_id);
	close(mirilla_fd);
	return 0;
}

/*
 * Place the peephole view at a caller-chosen address with `MAP_FIXED`,
 * disjoint from the observed range.
 */
static int test_self_peephole_map_fixed(void)
{
	int mirilla_fd = mirilla_open_device();
	ASSERT(mirilla_fd >= 0, "failed to open device");

	void *region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_2);
	ASSERT(region != NULL, "failed to allocate test region");

	/* Reserve a scratch range to give `MAP_FIXED` a well-defined home. */
	void *scratch = mmap(NULL, TEST_REGION_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT(scratch != MAP_FAILED, "failed to reserve scratch range");

	mirilla_map_target_id_t target_id = 0;
	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)),
	       "self-engage failed");

	mirilla_map_peephole_id_t peephole_id = 0;
	int peephole_fd = -1;
	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_peephole(
		       mirilla_fd, target_id, (virtual_address_t)region,
		       (virtual_address_t)region + TEST_REGION_SIZE, &peephole_id, &peephole_fd)),
	       "self-peephole failed");

	void *view = mmap(scratch, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE | MAP_FIXED,
			  peephole_fd, 0);
	ASSERT(view != MAP_FAILED, "MAP_FIXED peephole view failed");
	ASSERT(view == scratch, "MAP_FIXED view landed at the wrong address");

	print_with_timestamp("[SELF] Region at %p mirrored at fixed address %p\n", region, view);

	ASSERT(verify_region_faultable(view, TEST_REGION_SIZE, MAGIC_VALUE_2) == 0,
	       "fixed view does not mirror the observed region");

	munmap(view, TEST_REGION_SIZE);
	close(peephole_fd);
	munmap(region, TEST_REGION_SIZE);
	mirilla_disengage(mirilla_fd, target_id);
	close(mirilla_fd);
	return 0;
}

/*
 * The degenerate self-observation: `MAP_FIXED` the peephole view over the
 * observed range itself, so the peephole observes the range its own view
 * occupies. The access must fault, and the module must survive it.
 */
static int test_self_peephole_recursive_map_fixed(void)
{
	int mirilla_fd = mirilla_open_device();
	ASSERT(mirilla_fd >= 0, "failed to open device");

	void *region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_3);
	ASSERT(region != NULL, "failed to allocate test region");

	mirilla_map_target_id_t target_id = 0;
	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)),
	       "self-engage failed");

	mirilla_map_peephole_id_t peephole_id = 0;
	int peephole_fd = -1;
	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_peephole(
		       mirilla_fd, target_id, (virtual_address_t)region,
		       (virtual_address_t)region + TEST_REGION_SIZE, &peephole_id, &peephole_fd)),
	       "self-peephole failed");

	/*
	 * `MAP_FIXED` first unmaps the anonymous pages backing the range
	 * (the interval notifier zaps any installed view pages; the peephole
	 * stays alive), then installs the peephole VMA in their place.
	 */
	void *view = mmap(region, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE | MAP_FIXED,
			  peephole_fd, 0);
	ASSERT(view != MAP_FAILED, "recursive MAP_FIXED peephole view failed");
	ASSERT(view == region, "recursive view landed at the wrong address");

	print_with_timestamp("[SELF] Peephole view placed over its own observed range at %p\n",
			     view);

	/*
	 * NOTE(recursion): The probe below resolves the observed address back
	 * to the peephole VMA itself. GUP refuses `VM_IO` mappings outright,
	 * so the fault cannot recurse: it fails, the module answers SIGSEGV,
	 * and the protected read reports the error.
	 */
	unsigned int probe_value = 0;
	ASSERT(faultable_probe_u32(view, &probe_value) == CATALEJO_OUTCOME_ERROR,
	       "recursive view read succeeded; expected a fault");

	munmap(view, TEST_REGION_SIZE);
	close(peephole_fd);
	mirilla_disengage(mirilla_fd, target_id);
	close(mirilla_fd);

	/* The module must remain healthy after the recursive fault. */
	mirilla_fd = mirilla_open_device();
	ASSERT(mirilla_fd >= 0, "device is unusable after the recursive fault");

	void *fresh_region = allocate_test_region(TEST_REGION_SMALL, MAGIC_VALUE_4);
	ASSERT(fresh_region != NULL, "failed to allocate test region");

	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)),
	       "self-engage failed after the recursive fault");

	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_peephole(mirilla_fd, target_id,
						      (virtual_address_t)fresh_region,
						      (virtual_address_t)fresh_region +
							      TEST_REGION_SMALL,
						      &peephole_id, &peephole_fd)),
	       "self-peephole failed after the recursive fault");

	void *fresh_view = mmap(NULL, TEST_REGION_SMALL, PROT_READ, MAP_PRIVATE, peephole_fd, 0);
	ASSERT(fresh_view != MAP_FAILED, "failed to mmap peephole view after the recursive fault");
	ASSERT(verify_region_faultable(fresh_view, TEST_REGION_SMALL, MAGIC_VALUE_4) == 0,
	       "view does not mirror the observed region after the recursive fault");

	munmap(fresh_view, TEST_REGION_SMALL);
	close(peephole_fd);
	munmap(fresh_region, TEST_REGION_SMALL);
	mirilla_disengage(mirilla_fd, target_id);
	close(mirilla_fd);
	return 0;
}

/*
 * Clobber the observed range with fresh anonymous memory via `MAP_FIXED`.
 * The peephole stays alive across the unmap and its next fault re-resolves
 * the address, so the view must reflect the new contents.
 */
static int test_self_map_fixed_clobber_reflects_new(void)
{
	int mirilla_fd = mirilla_open_device();
	ASSERT(mirilla_fd >= 0, "failed to open device");

	void *region = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_1);
	ASSERT(region != NULL, "failed to allocate test region");

	mirilla_map_target_id_t target_id = 0;
	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_engage(mirilla_fd, getpid(), &target_id)),
	       "self-engage failed");

	mirilla_map_peephole_id_t peephole_id = 0;
	int peephole_fd = -1;
	ASSERT(MIRILLA_COMMAND_IS_OK(mirilla_peephole(
		       mirilla_fd, target_id, (virtual_address_t)region,
		       (virtual_address_t)region + TEST_REGION_SIZE, &peephole_id, &peephole_fd)),
	       "self-peephole failed");

	void *view = mmap(NULL, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE, peephole_fd, 0);
	ASSERT(view != MAP_FAILED, "failed to mmap peephole view");

	ASSERT(verify_region_faultable(view, TEST_REGION_SIZE, MAGIC_VALUE_1) == 0,
	       "view does not mirror the initial contents");

	/*
	 * Replace the observed range wholesale. The interval notifier zaps
	 * the installed view pages but the peephole stays alive: it tracks
	 * the address range, not the mapping that used to back it.
	 */
	void *clobber = mmap(region, TEST_REGION_SIZE, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT(clobber != MAP_FAILED, "MAP_FIXED clobber failed");
	ASSERT(clobber == region, "MAP_FIXED clobber landed at the wrong address");

	unsigned int *data = (unsigned int *)clobber;
	for (size_t i = 0; i < TEST_REGION_SIZE / sizeof(unsigned int); i++)
		data[i] = MAGIC_VALUE_5 + i;

	print_with_timestamp("[SELF] Observed range clobbered with a fresh pattern\n");

	/* The next view faults re-resolve the range to the new memory. */
	ASSERT(verify_region_faultable(view, TEST_REGION_SIZE, MAGIC_VALUE_5) == 0,
	       "view does not reflect the clobbered contents");

	munmap(view, TEST_REGION_SIZE);
	close(peephole_fd);
	munmap(region, TEST_REGION_SIZE);
	mirilla_disengage(mirilla_fd, target_id);
	close(mirilla_fd);
	return 0;
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
	print_banner("MIRILLA SELF-OBSERVATION SUITE");

	if (harness_fault_initialize() < 0)
		return EXIT_FAILURE;

	RUN_TEST("Self Engage", test_self_engage);
	RUN_TEST("Self Peephole - Basic", test_self_peephole_basic);
	RUN_TEST("Self Peephole - MAP_FIXED View", test_self_peephole_map_fixed);
	RUN_TEST("Self Peephole - Recursive MAP_FIXED - Fault Expected",
		 test_self_peephole_recursive_map_fixed);
	RUN_TEST("Self Peephole - MAP_FIXED Clobber Reflects New Contents",
		 test_self_map_fixed_clobber_reflects_new);

	print_summary();

	return suite_status();
}
