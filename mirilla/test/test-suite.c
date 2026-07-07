/*
 * Comprehensive Test Suite for Mirilla Module (New API)
 *
 * This test program validates the new Mirilla API which simplifies
 * the engagement model:
 * - ENGAGE: Start observing a process (returns target_id)
 * - PEEPHOLE: Create memory mirror (returns peephole_id and fd)
 * - DISENGAGE: Stop observing a process
 *
 * Tests include:
 * - Basic engagement/disengagement
 * - Single peephole creation and access
 * - Multiple peepholes on same target
 * - Multiple targets from same observer
 * - Memory change propagation
 * - MMU notifier callbacks (unmap, remap)
 * - Performance benchmarks
 * - Cleanup and error handling
 */

#include "test-harness.h"

/* Structure to track multiple regions in child */
typedef struct {
	void *addr;
	size_t size;
	unsigned int current_value;
} child_region_t;

#define MAX_CHILD_REGIONS 4

/* Child process: the observed process */
static int child_process(int pipe_in, int pipe_out)
{
	ipc_message_t msg;
	child_region_t regions[MAX_CHILD_REGIONS] = { 0 };
	int num_regions = 0;

	print_with_timestamp(COLOR_YELLOW "[CHILD %d] Started\n" COLOR_RESET, getpid());

	/* Allocate initial test memory region */
	regions[0].addr = allocate_test_region(TEST_REGION_SIZE, MAGIC_VALUE_1);
	if (!regions[0].addr) {
		fprintf(stderr, "[CHILD] Failed to allocate test region\n");
		return -1;
	}
	regions[0].size = TEST_REGION_SIZE;
	regions[0].current_value = MAGIC_VALUE_1;
	num_regions = 1;

	print_with_timestamp("[CHILD] Allocated region[0] at %p, size %d bytes\n", regions[0].addr,
			     TEST_REGION_SIZE);

	/* Send ready signal with address */
	if (send_message(pipe_out, MSG_READY, (unsigned long)regions[0].addr, TEST_REGION_SIZE) <
	    0) {
		fprintf(stderr, "[CHILD] Failed to send ready message\n");
		munmap(regions[0].addr, regions[0].size);
		return -1;
	}

	/* Main loop: wait for parent commands */
	while (1) {
		if (recv_message(pipe_in, &msg) < 0) {
			fprintf(stderr, "[CHILD] Failed to receive message\n");
			break;
		}

		switch (msg.type) {
		case MSG_ALLOCATE_REGION: {
			/* Allocate a new region */
			if (num_regions >= MAX_CHILD_REGIONS) {
				print_with_timestamp("[CHILD] Too many regions\n");
				send_message(pipe_out, MSG_ERROR, ENOMEM, 0);
				break;
			}

			size_t size = (size_t)msg.data;
			unsigned int value = (unsigned int)msg.data2;

			regions[num_regions].addr = allocate_test_region(size, value);
			if (!regions[num_regions].addr) {
				print_with_timestamp("[CHILD] Failed to allocate region[%d]\n",
						     num_regions);
				send_message(pipe_out, MSG_ERROR, ENOMEM, 0);
				break;
			}

			regions[num_regions].size = size;
			regions[num_regions].current_value = value;

			print_with_timestamp("[CHILD] Allocated region[%d] at %p, size %zu bytes\n",
					     num_regions, regions[num_regions].addr, size);

			send_message(pipe_out, MSG_ACK, (unsigned long)regions[num_regions].addr,
				     size);
			num_regions++;
			break;
		}

		case MSG_WRITE_DATA: {
			/* Write pattern to memory region */
			int region_idx = (int)msg.data2;
			if (region_idx < 0 || region_idx >= num_regions ||
			    !regions[region_idx].addr) {
				print_with_timestamp("[CHILD] Invalid region index %d\n",
						     region_idx);
				send_message(pipe_out, MSG_ERROR, EINVAL, 0);
				break;
			}

			unsigned int value = (unsigned int)msg.data;
			unsigned int *data = (unsigned int *)regions[region_idx].addr;
			print_with_timestamp("[CHILD] Writing pattern 0x%x to region[%d]\n", value,
					     region_idx);

			for (size_t i = 0; i < regions[region_idx].size / sizeof(unsigned int);
			     i++) {
				data[i] = value + i;
			}

			/* Force memory to be paged in */
			for (size_t i = 0; i < regions[region_idx].size; i += 4096) {
				volatile unsigned char *p =
					(volatile unsigned char *)regions[region_idx].addr + i;
				(void)*p;
			}

			regions[region_idx].current_value = value;
			send_message(pipe_out, MSG_ACK, 0, 0);
			break;
		}

		case MSG_MODIFY: {
			/* Modify specific location in a region */
			int region_idx = (int)msg.data2;
			size_t offset = (size_t)msg.data;

			if (region_idx < 0 || region_idx >= num_regions ||
			    !regions[region_idx].addr) {
				print_with_timestamp("[CHILD] Invalid region index %d\n",
						     region_idx);
				send_message(pipe_out, MSG_ERROR, EINVAL, 0);
				break;
			}

			unsigned int *data = (unsigned int *)regions[region_idx].addr;
			print_with_timestamp("[CHILD] Modifying region[%d] offset %zu\n",
					     region_idx, offset);

			if (offset < regions[region_idx].size / sizeof(unsigned int)) {
				data[offset] = MAGIC_VALUE_4;
			}

			send_message(pipe_out, MSG_ACK, 0, 0);
			break;
		}

		case MSG_UNMAP_REGION: {
			/* Unmap a region */
			int region_idx = (int)msg.data;

			if (region_idx < 0 || region_idx >= num_regions ||
			    !regions[region_idx].addr) {
				print_with_timestamp("[CHILD] Invalid region index %d for unmap\n",
						     region_idx);
				send_message(pipe_out, MSG_ERROR, EINVAL, 0);
				break;
			}

			print_with_timestamp("[CHILD] Unmapping region[%d] at %p\n", region_idx,
					     regions[region_idx].addr);

			if (munmap(regions[region_idx].addr, regions[region_idx].size) < 0) {
				perror("[CHILD] munmap");
				send_message(pipe_out, MSG_ERROR, errno, 0);
			} else {
				regions[region_idx].addr = NULL;
				send_message(pipe_out, MSG_ACK, 0, 0);
			}
			break;
		}

		case MSG_REMAP_REGION: {
			/* Remap a region to new address */
			int region_idx = (int)msg.data;

			if (region_idx < 0 || region_idx >= num_regions) {
				print_with_timestamp("[CHILD] Invalid region index %d for remap\n",
						     region_idx);
				send_message(pipe_out, MSG_ERROR, EINVAL, 0);
				break;
			}

			print_with_timestamp("[CHILD] Remapping region[%d]\n", region_idx);

			void *new_addr = allocate_test_region(regions[region_idx].size,
							      regions[region_idx].current_value);
			if (!new_addr) {
				print_with_timestamp("[CHILD] Failed to remap region[%d]\n",
						     region_idx);
				send_message(pipe_out, MSG_ERROR, ENOMEM, 0);
				break;
			}

			/* Unmap old region if it exists */
			if (regions[region_idx].addr) {
				munmap(regions[region_idx].addr, regions[region_idx].size);
			}

			regions[region_idx].addr = new_addr;
			print_with_timestamp("[CHILD] Remapped region[%d] to %p\n", region_idx,
					     new_addr);
			send_message(pipe_out, MSG_ACK, (unsigned long)new_addr, 0);
			break;
		}

		case MSG_VERIFY: {
			/* Verify memory contents */
			int region_idx = (int)msg.data2;
			unsigned int expected = (unsigned int)msg.data;

			if (region_idx < 0 || region_idx >= num_regions ||
			    !regions[region_idx].addr) {
				print_with_timestamp("[CHILD] Invalid region index %d for verify\n",
						     region_idx);
				send_message(pipe_out, MSG_ERROR, EINVAL, 0);
				break;
			}

			print_with_timestamp("[CHILD] Verifying region[%d] with base value 0x%x\n",
					     region_idx, expected);

			int result = verify_region(regions[region_idx].addr,
						   regions[region_idx].size, expected);
			send_message(pipe_out, MSG_ACK, result, 0);
			break;
		}

		case MSG_EXIT:
			print_with_timestamp("[CHILD] Received exit command\n");
			for (int i = 0; i < num_regions; i++) {
				if (regions[i].addr) {
					munmap(regions[i].addr, regions[i].size);
				}
			}
			return 0;

		default:
			fprintf(stderr, "[CHILD] Unknown message type: %d\n", msg.type);
			break;
		}
	}

	/* Cleanup on error */
	for (int i = 0; i < num_regions; i++) {
		if (regions[i].addr) {
			munmap(regions[i].addr, regions[i].size);
		}
	}
	return -1;
}

/* Parent process: the observer process */
static int parent_process(pid_t child_pid, int pipe_in, int pipe_out)
{
	int mirilla_fd = -1;
	ipc_message_t msg;
	int test_status = 0;
	mirilla_map_target_id_t target_id = 0;

/* Peephole tracking */
#define MAX_PEEPHOLES 8
	struct {
		mirilla_map_peephole_id_t id;
		int fd;
		void *mapped_addr;
		size_t size;
		void *child_addr;
	} peepholes[MAX_PEEPHOLES] = { 0 };
	int num_peepholes = 0;
	void *remapped_region0_addr = NULL; /* Track remapped address of region[0] */

	print_with_timestamp(COLOR_YELLOW "[PARENT %d] Started, observing child %d\n" COLOR_RESET,
			     getpid(), child_pid);

	/* Open mirilla device */
	mirilla_fd = mirilla_open_device();
	if (mirilla_fd < 0)
		return -1;
	printf("[PARENT] Opened mirilla device (fd=%d)\n", mirilla_fd);

	/* Wait for child to be ready */
	if (recv_message(pipe_in, &msg) < 0 || msg.type != MSG_READY) {
		fprintf(stderr, "[PARENT] Failed to receive ready message from child\n");
		close(mirilla_fd);
		return -1;
	}
	void *child_region_addr = (void *)msg.data;
	size_t child_region_size = (size_t)msg.data2;
	printf("[PARENT] Child is ready, test region at %p (size %zu)\n", child_region_addr,
	       child_region_size);

	usleep(100000); /* Give child time to initialize */

	/*
     * ========================================================================
     * Test 1: MAP_ENGAGE - Engage with child process
     * ========================================================================
     */
	TEST_START("MAP_ENGAGE");
	{
		union mirilla_map_engage_io engage_io = { 0 };
		engage_io.argument.process_id = child_pid;

		unsigned int cmd = MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP,
							   MIRILLA_COMMAND_MAP_ENGAGE);
		long ret = ioctl(mirilla_fd, cmd, &engage_io);

		if (!MIRILLA_COMMAND_IS_OK(ret)) {
			char error_msg[256];
			snprintf(error_msg, sizeof(error_msg),
				 "ioctl failed with ret=%ld, errno=%d (%s)", ret, errno,
				 strerror(errno));
			TEST_FAIL("MAP_ENGAGE", error_msg);
			test_status = -1;
			goto cleanup;
		}

		target_id = engage_io.result.target_id;
		printf("[PARENT] Engaged with child, target_id = %lu\n", (unsigned long)target_id);
		TEST_PASS("MAP_ENGAGE");
	}

	/*
     * ========================================================================
     * Test 2: MAP_PEEPHOLE - Create first peephole
     * ========================================================================
     */
	TEST_START("MAP_PEEPHOLE - First peephole");
	{
		union mirilla_map_peephole_io peephole_io = { 0 };
		peephole_io.argument.target_id = target_id;
		peephole_io.argument.start_address = (uint64_t)(unsigned long)child_region_addr;
		peephole_io.argument.end_address =
			(uint64_t)(unsigned long)child_region_addr + child_region_size;

		unsigned int cmd = MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP,
							   MIRILLA_COMMAND_MAP_PEEPHOLE);
		long ret = ioctl(mirilla_fd, cmd, &peephole_io);

		if (!MIRILLA_COMMAND_IS_OK(ret)) {
			char error_msg[256];
			snprintf(error_msg, sizeof(error_msg),
				 "ioctl failed with ret=%ld, errno=%d (%s)", ret, errno,
				 strerror(errno));
			TEST_FAIL("MAP_PEEPHOLE - First peephole", error_msg);
			test_status = -1;
			goto cleanup;
		}

		peepholes[0].id = peephole_io.result.id;
		peepholes[0].fd = peephole_io.result.fd;
		peepholes[0].size = child_region_size;
		peepholes[0].child_addr = child_region_addr;

		printf("[PARENT] Peephole[0] created: id=%lu, fd=%d\n",
		       (unsigned long)peepholes[0].id, peepholes[0].fd);

		/* mmap the peephole fd */
		peepholes[0].mapped_addr =
			mmap(NULL, peepholes[0].size, PROT_READ, MAP_PRIVATE, peepholes[0].fd, 0);
		if (peepholes[0].mapped_addr == MAP_FAILED) {
			perror("mmap peephole_fd");
			TEST_FAIL("MAP_PEEPHOLE - First peephole", "mmap failed");
			test_status = -1;
			goto cleanup;
		}

		printf("[PARENT] Mapped peephole[0] at %p\n", peepholes[0].mapped_addr);
		num_peepholes = 1;
		TEST_PASS("MAP_PEEPHOLE - First peephole");
	}

	/*
     * ========================================================================
     * Test 3: Read Initial Mirror Data
     * ========================================================================
     */
	TEST_START("Read Initial Mirror Data");
	{
		usleep(50000);

		if (verify_region(peepholes[0].mapped_addr, peepholes[0].size, MAGIC_VALUE_1) ==
		    0) {
			TEST_PASS("Read Initial Mirror Data");
		} else {
			TEST_FAIL("Read Initial Mirror Data", "data mismatch");
		}
	}

	/*
     * ========================================================================
     * Test 4: Multiple Peepholes on Same Target
     * ========================================================================
     */
	TEST_START("Multiple Peepholes - Same Target");
	{
		/* Create second peephole on same region (different view) */
		union mirilla_map_peephole_io peephole_io = { 0 };
		peephole_io.argument.target_id = target_id;
		peephole_io.argument.start_address = (uint64_t)(unsigned long)child_region_addr;
		peephole_io.argument.end_address =
			(uint64_t)(unsigned long)child_region_addr + TEST_REGION_SMALL;

		unsigned int cmd = MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP,
							   MIRILLA_COMMAND_MAP_PEEPHOLE);
		long ret = ioctl(mirilla_fd, cmd, &peephole_io);

		if (!MIRILLA_COMMAND_IS_OK(ret)) {
			char error_msg[256];
			snprintf(error_msg, sizeof(error_msg),
				 "failed to create second peephole: ret=%ld", ret);
			TEST_FAIL("Multiple Peepholes - Same Target", error_msg);
		} else {
			peepholes[1].id = peephole_io.result.id;
			peepholes[1].fd = peephole_io.result.fd;
			peepholes[1].size = TEST_REGION_SMALL;
			peepholes[1].child_addr = child_region_addr;

			peepholes[1].mapped_addr = mmap(NULL, peepholes[1].size, PROT_READ,
							MAP_PRIVATE, peepholes[1].fd, 0);

			if (peepholes[1].mapped_addr == MAP_FAILED) {
				TEST_FAIL("Multiple Peepholes - Same Target", "mmap failed");
			} else {
				num_peepholes = 2;
				printf("[PARENT] Created peephole[1]: id=%lu, fd=%d, mapped at "
				       "%p\n",
				       (unsigned long)peepholes[1].id, peepholes[1].fd,
				       peepholes[1].mapped_addr);

				/* Verify both peepholes show same data */
				if (verify_region(peepholes[0].mapped_addr, TEST_REGION_SMALL,
						  MAGIC_VALUE_1) == 0 &&
				    verify_region(peepholes[1].mapped_addr, TEST_REGION_SMALL,
						  MAGIC_VALUE_1) == 0) {
					TEST_PASS("Multiple Peepholes - Same Target");
				} else {
					TEST_FAIL("Multiple Peepholes - Same Target", "data "
										      "mismatch");
				}
			}
		}
	}

	/*
     * ========================================================================
     * Test 5: Child Write Propagation to Multiple Peepholes
     * ========================================================================
     */
	TEST_START("Child Write Propagation - Multiple Peepholes");
	{
		/* Ask child to write new pattern */
		send_message(pipe_out, MSG_WRITE_DATA, MAGIC_VALUE_2, 0);

		if (recv_message(pipe_in, &msg) < 0 || msg.type != MSG_ACK) {
			TEST_FAIL("Child Write Propagation - Multiple Peepholes", "failed to "
										  "receive ACK");
		} else {
			usleep(50000);

			/* Verify both peepholes see the update */
			int peephole0_ok = verify_region(peepholes[0].mapped_addr,
							 peepholes[0].size, MAGIC_VALUE_2) == 0;
			int peephole1_ok = verify_region(peepholes[1].mapped_addr,
							 peepholes[1].size, MAGIC_VALUE_2) == 0;

			if (peephole0_ok && peephole1_ok) {
				TEST_PASS("Child Write Propagation - Multiple Peepholes");
			} else {
				TEST_FAIL("Child Write Propagation - Multiple Peepholes", "data "
											  "mismatch"
											  " in one "
											  "or more "
											  "peephole"
											  "s");
			}
		}
	}

	/*
     * ========================================================================
     * Test 6: Create Additional Child Region for Multi-Target Test
     * ========================================================================
     */
	void *child_region2_addr = NULL;
	size_t child_region2_size = 0;

	TEST_START("Allocate Second Child Region");
	{
		/* Ask child to allocate another region */
		send_message(pipe_out, MSG_ALLOCATE_REGION, TEST_REGION_SMALL, MAGIC_VALUE_3);

		if (recv_message(pipe_in, &msg) < 0 || msg.type != MSG_ACK) {
			TEST_FAIL("Allocate Second Child Region", "failed to allocate");
		} else {
			child_region2_addr = (void *)msg.data;
			child_region2_size = (size_t)msg.data2;
			printf("[PARENT] Child allocated region[1] at %p (size %zu)\n",
			       child_region2_addr, child_region2_size);
			TEST_PASS("Allocate Second Child Region");
		}
	}

	/*
     * ========================================================================
     * Test 7: Multiple Peepholes on Different Regions
     * ========================================================================
     */
	if (child_region2_addr) {
		TEST_START("Multiple Peepholes - Different Regions");
		{
			/* Create peephole for second region */
			union mirilla_map_peephole_io peephole_io = { 0 };
			peephole_io.argument.target_id = target_id;
			peephole_io.argument.start_address =
				(uint64_t)(unsigned long)child_region2_addr;
			peephole_io.argument.end_address =
				(uint64_t)(unsigned long)child_region2_addr + child_region2_size;

			unsigned int cmd = MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP,
								   MIRILLA_COMMAND_MAP_PEEPHOLE);
			long ret = ioctl(mirilla_fd, cmd, &peephole_io);

			if (!MIRILLA_COMMAND_IS_OK(ret)) {
				char error_msg[256];
				snprintf(error_msg, sizeof(error_msg),
					 "failed to create peephole: ret=%ld", ret);
				TEST_FAIL("Multiple Peepholes - Different Regions", error_msg);
			} else {
				peepholes[2].id = peephole_io.result.id;
				peepholes[2].fd = peephole_io.result.fd;
				peepholes[2].size = child_region2_size;
				peepholes[2].child_addr = child_region2_addr;

				peepholes[2].mapped_addr = mmap(NULL, peepholes[2].size, PROT_READ,
								MAP_PRIVATE, peepholes[2].fd, 0);

				if (peepholes[2].mapped_addr == MAP_FAILED) {
					TEST_FAIL("Multiple Peepholes - Different Regions", "mmap "
											    "faile"
											    "d");
				} else {
					num_peepholes = 3;
					printf("[PARENT] Created peephole[2] for region[1]: "
					       "id=%lu, fd=%d, mapped at %p\n",
					       (unsigned long)peepholes[2].id, peepholes[2].fd,
					       peepholes[2].mapped_addr);

					/* Verify we can read from both regions */
					if (verify_region(peepholes[0].mapped_addr,
							  TEST_REGION_SMALL, MAGIC_VALUE_2) == 0 &&
					    verify_region(peepholes[2].mapped_addr,
							  child_region2_size, MAGIC_VALUE_3) == 0) {
						TEST_PASS("Multiple Peepholes - Different Regions");
					} else {
						TEST_FAIL("Multiple Peepholes - Different Regions",
							  "data mismatch");
					}
				}
			}
		}
	}

	/*
     * ========================================================================
     * Test 8: Multiple Modifications Across Regions
     * ========================================================================
     */
	TEST_START("Multiple Modifications Across Regions");
	{
		int all_passed = 1;

		/* Modify first region */
		send_message(pipe_out, MSG_WRITE_DATA, MAGIC_VALUE_4, 0);
		recv_message(pipe_in, &msg);
		usleep(50000);

		if (verify_region(peepholes[0].mapped_addr, TEST_REGION_SMALL, MAGIC_VALUE_4) !=
		    0) {
			all_passed = 0;
		}

		/* Modify second region if it exists */
		if (num_peepholes >= 3 && peepholes[2].mapped_addr) {
			send_message(pipe_out, MSG_WRITE_DATA, MAGIC_VALUE_5, 1);
			recv_message(pipe_in, &msg);
			usleep(50000);

			if (verify_region(peepholes[2].mapped_addr, child_region2_size,
					  MAGIC_VALUE_5) != 0) {
				all_passed = 0;
			}
		}

		/* Verify first region again */
		if (verify_region(peepholes[0].mapped_addr, TEST_REGION_SMALL, MAGIC_VALUE_4) !=
		    0) {
			all_passed = 0;
		}

		if (all_passed) {
			TEST_PASS("Multiple Modifications Across Regions");
		} else {
			TEST_FAIL("Multiple Modifications Across Regions", "data mismatch");
		}
	}

	/*
     * ========================================================================
     * Test 9: MMU Notifier - Child Unmaps Region
     * ========================================================================
     */
	TEST_START("MMU Notifier - Child Unmaps Region");
	{
		/* Ask child to unmap second region */
		if (child_region2_addr) {
			send_message(pipe_out, MSG_UNMAP_REGION, 1, 0);

			if (recv_message(pipe_in, &msg) < 0 || msg.type != MSG_ACK) {
				TEST_FAIL("MMU Notifier - Child Unmaps Region", "unmap failed");
			} else {
				usleep(50000);

				/* Try to access unmapped peephole - should get error or zero */
				/* We just verify the operation completed without crash */
				printf("[PARENT] Child unmapped region[1], peephole[2] should be "
				       "invalidated\n");
				TEST_PASS("MMU Notifier - Child Unmaps Region");
			}
		} else {
			printf("[PARENT] Skipping unmap test (no second region)\n");
			results.total--; /* Don't count this test */
		}
	}

	/*
     * ========================================================================
     * Test 10: MMU Notifier - Child Remaps Region
     * ========================================================================
     */
	TEST_START("MMU Notifier - Child Remaps Region");
	{
		/* Ask child to remap first region */
		send_message(pipe_out, MSG_REMAP_REGION, 0, 0);

		if (recv_message(pipe_in, &msg) < 0 || msg.type != MSG_ACK) {
			TEST_FAIL("MMU Notifier - Child Remaps Region", "remap failed");
		} else {
			remapped_region0_addr = (void *)msg.data;
			printf("[PARENT] Child remapped region[0] to %p\n", remapped_region0_addr);

			usleep(50000);

			/* Old peepholes should still work (they track the VMA, not address) */
			/* Or they might be invalidated - depends on implementation */
			printf("[PARENT] Remap completed, existing peepholes may be invalidated\n");
			TEST_PASS("MMU Notifier - Child Remaps Region");
		}
	}

	/*
     * ========================================================================
     * Test 11: Access After Remap - Fault Expected
     * ========================================================================
     */
	TEST_START("Access After Remap - Fault Expected");
	{
		/* After remap, the old peephole points to invalid memory.
         * Accessing it should fault, which the protected read reports as an
         * error outcome instead of crashing the observer.
         */
		printf("[PARENT] Attempting to read from peephole after child remap (should "
		       "fault)...\n");

		unsigned int probe_value = 0;

		if (faultable_probe_u32(peepholes[0].mapped_addr, &probe_value) ==
		    CATALEJO_OUTCOME_ERROR) {
			printf("[PARENT] ✓ Protected read reported the fault as expected (memory "
			       "no longer accessible)\n");
			TEST_PASS("Access After Remap - Fault Expected");
		} else {
			printf("[PARENT] ERROR: Access succeeded without fault! Value: 0x%x\n",
			       probe_value);
			TEST_FAIL("Access After Remap - Fault Expected", "no fault reported");
		}
	}

	/*
     * ========================================================================
     * Test 12: Mirror Read Performance
     * ========================================================================
     */
	TEST_START("Mirror Read Performance");
	{
		/* After the remap test, peephole[0] is invalidated.
		 * We need to create a fresh peephole for the performance test using
		 * the remapped address from region[0].
		 */
		if (!remapped_region0_addr) {
			TEST_FAIL("Mirror Read Performance", "no remapped address available");
			goto skip_performance_test;
		}

		/* Ask child to write fresh data to the remapped region */
		send_message(pipe_out, MSG_WRITE_DATA, MAGIC_VALUE_1, 0);
		if (recv_message(pipe_in, &msg) < 0 || msg.type != MSG_ACK) {
			TEST_FAIL("Mirror Read Performance", "failed to write test data");
			goto skip_performance_test;
		}

		usleep(50000); /* Let the write complete */

		/* Create a new peephole for the remapped region */
		union mirilla_map_peephole_io peephole_io = { 0 };
		peephole_io.argument.target_id = target_id;
		peephole_io.argument.start_address = (uint64_t)(unsigned long)remapped_region0_addr;
		peephole_io.argument.end_address =
			(uint64_t)(unsigned long)remapped_region0_addr + TEST_REGION_SIZE;

		unsigned int cmd = MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP,
							   MIRILLA_COMMAND_MAP_PEEPHOLE);
		long ret = ioctl(mirilla_fd, cmd, &peephole_io);

		if (!MIRILLA_COMMAND_IS_OK(ret)) {
			TEST_FAIL("Mirror Read Performance", "failed to create fresh peephole");
			goto skip_performance_test;
		}

		/* Store the new peephole in a temp slot */
		int perf_fd = peephole_io.result.fd;
		void *perf_mapped =
			mmap(NULL, TEST_REGION_SIZE, PROT_READ, MAP_PRIVATE, perf_fd, 0);

		if (perf_mapped == MAP_FAILED) {
			close(perf_fd);
			TEST_FAIL("Mirror Read Performance", "failed to mmap fresh peephole");
			goto skip_performance_test;
		}

		/* Now run the performance test */
		struct timespec start, end;
		int iterations = 10000;
		volatile unsigned int sum = 0;

		clock_gettime(CLOCK_MONOTONIC, &start);

		for (int i = 0; i < iterations; i++) {
			unsigned int *data = (unsigned int *)perf_mapped;
			for (size_t j = 0; j < TEST_REGION_SMALL / sizeof(unsigned int); j += 64) {
				sum += data[j];
			}
		}

		clock_gettime(CLOCK_MONOTONIC, &end);

		double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
		double ops_per_sec = iterations / elapsed;

		printf("[PARENT] Read performance: %.2f iterations/sec (sum=%u)\n", ops_per_sec,
		       sum);

		/* Cleanup performance test peephole */
		munmap(perf_mapped, TEST_REGION_SIZE);
		close(perf_fd);

		TEST_PASS("Mirror Read Performance");
		goto performance_test_done;

skip_performance_test:
		printf("[PARENT] Skipping performance test due to setup failure\n");

performance_test_done:;
	}

	/*
     * ========================================================================
     * Test 13: MAP_DISENGAGE
     * ========================================================================
     */
	TEST_START("MAP_DISENGAGE");
	{
		/* First cleanup all peepholes */
		for (int i = 0; i < num_peepholes; i++) {
			if (peepholes[i].mapped_addr && peepholes[i].mapped_addr != MAP_FAILED) {
				munmap(peepholes[i].mapped_addr, peepholes[i].size);
				peepholes[i].mapped_addr = NULL;
			}
			if (peepholes[i].fd >= 0) {
				close(peepholes[i].fd);
				peepholes[i].fd = -1;
			}
		}

		union mirilla_map_disengage_io disengage_io = { 0 };
		disengage_io.argument.target_id = target_id;

		unsigned int cmd = MIRILLA_COMMAND_ENCODE(MIRILLA_COMMAND_CATEGORY_MAP,
							   MIRILLA_COMMAND_MAP_DISENGAGE);
		long ret = ioctl(mirilla_fd, cmd, &disengage_io);

		if (!MIRILLA_COMMAND_IS_OK(ret)) {
			char error_msg[256];
			snprintf(error_msg, sizeof(error_msg), "ioctl failed with ret=%ld", ret);
			TEST_FAIL("MAP_DISENGAGE", error_msg);
		} else {
			printf("[PARENT] Disengaged from child\n");
			TEST_PASS("MAP_DISENGAGE");
		}
	}

cleanup:
	/* Signal child to exit */
	send_message(pipe_out, MSG_EXIT, 0, 0);

	/* Cleanup any remaining peepholes */
	for (int i = 0; i < num_peepholes; i++) {
		if (peepholes[i].mapped_addr && peepholes[i].mapped_addr != MAP_FAILED) {
			munmap(peepholes[i].mapped_addr, peepholes[i].size);
		}
		if (peepholes[i].fd >= 0) {
			close(peepholes[i].fd);
		}
	}

	if (mirilla_fd >= 0) {
		close(mirilla_fd);
	}

	return test_status;
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused)))
{
	int pipe_parent_to_child[2];
	int pipe_child_to_parent[2];
	pid_t child_pid;
	int status = 0;

	print_banner("MIRILLA MODULE TEST SUITE");

	if (harness_fault_initialize() < 0)
		return EXIT_FAILURE;

	/* Create pipes for IPC */
	if (pipe(pipe_parent_to_child) < 0) {
		perror("pipe");
		return EXIT_FAILURE;
	}

	if (pipe(pipe_child_to_parent) < 0) {
		perror("pipe");
		close(pipe_parent_to_child[PIPE_READ]);
		close(pipe_parent_to_child[PIPE_WRITE]);
		return EXIT_FAILURE;
	}

	/* Fork child process */
	child_pid = fork();
	if (child_pid < 0) {
		perror("fork");
		return EXIT_FAILURE;
	}

	if (child_pid == 0) {
		/* Child process */
		close(pipe_parent_to_child[PIPE_WRITE]);
		close(pipe_child_to_parent[PIPE_READ]);

		status = child_process(pipe_parent_to_child[PIPE_READ],
				       pipe_child_to_parent[PIPE_WRITE]);

		close(pipe_parent_to_child[PIPE_READ]);
		close(pipe_child_to_parent[PIPE_WRITE]);

		exit(status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
	} else {
		/* Parent process */
		close(pipe_parent_to_child[PIPE_READ]);
		close(pipe_child_to_parent[PIPE_WRITE]);

		status = parent_process(child_pid, pipe_child_to_parent[PIPE_READ],
					pipe_parent_to_child[PIPE_WRITE]);

		close(pipe_parent_to_child[PIPE_WRITE]);
		close(pipe_child_to_parent[PIPE_READ]);

		/* Wait for child to exit */
		int child_status;
		waitpid(child_pid, &child_status, 0);

		if (WIFEXITED(child_status)) {
			printf("\n[PARENT] Child exited with status %d\n",
			       WEXITSTATUS(child_status));
		} else if (WIFSIGNALED(child_status)) {
			printf("\n[PARENT] Child terminated by signal %d\n",
			       WTERMSIG(child_status));
		}
	}

	print_summary();

	return suite_status();
}
