/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE

#include <linux/limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../kselftest.h"
#include "cgroup_util.h"

/*
 * This test creates two nested cgroups with and without enabling
 * the memory controller.
 */
static int test_memcg_subtree_control(const char *root)
{
	char *parent, *child, *parent2, *child2;
	int ret = KSFT_FAIL;
	char buf[PAGE_SIZE];

	/* Create two nested cgroups with the memory controller enabled */
	parent = cg_name(root, "memcg_test_0");
	child = cg_name(root, "memcg_test_0/memcg_test_1");
	if (!parent || !child)
		goto cleanup;

	if (cg_create(parent))
		goto cleanup;

	if (cg_write(parent, "cgroup.subtree_control", "+memory"))
		goto cleanup;

	if (cg_create(child))
		goto cleanup;

	if (cg_read_strstr(child, "cgroup.controllers", "memory"))
		goto cleanup;

	/* Create two nested cgroups without enabling memory controller */
	parent2 = cg_name(root, "memcg_test_1");
	child2 = cg_name(root, "memcg_test_1/memcg_test_1");
	if (!parent2 || !child2)
		goto cleanup;

	if (cg_create(parent2))
		goto cleanup;

	if (cg_create(child2))
		goto cleanup;

	if (cg_read(child2, "cgroup.controllers", buf, sizeof(buf)))
		goto cleanup;

	if (!cg_read_strstr(child2, "cgroup.controllers", "memory"))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_destroy(child);
	cg_destroy(parent);
	free(parent);
	free(child);

	cg_destroy(child2);
	cg_destroy(parent2);
	free(parent2);
	free(child2);

	return ret;
}

static int alloc_anon_50M_check(const char *cgroup, void *arg)
{
	size_t size = MB(50);
	char *buf, *ptr;
	long anon, current;
	int ret = -1;

	buf = malloc(size);
	for (ptr = buf; ptr < buf + size; ptr += PAGE_SIZE)
		*ptr = 0;

	current = cg_read_long(cgroup, "memory.current");
	if (current < size)
		goto cleanup;

	if (!values_close(size, current, 3))
		goto cleanup;

	anon = cg_read_key_long(cgroup, "memory.stat", "anon ");
	if (anon < 0)
		goto cleanup;

	if (!values_close(anon, current, 3))
		goto cleanup;

	ret = 0;
cleanup:
	free(buf);
	return ret;
}

static int alloc_pagecache_50M_check(const char *cgroup, void *arg)
{
	size_t size = MB(50);
	int ret = -1;
	long current, file;
	int fd;

	fd = get_temp_fd();
	if (fd < 0)
		return -1;

	if (alloc_pagecache(fd, size))
		goto cleanup;

	current = cg_read_long(cgroup, "memory.current");
	if (current < size)
		goto cleanup;

	file = cg_read_key_long(cgroup, "memory.stat", "file ");
	if (file < 0)
		goto cleanup;

	if (!values_close(file, current, 10))
		goto cleanup;

	ret = 0;

cleanup:
	close(fd);
	return ret;
}

/*
 * This test create a memory cgroup, allocates
 * some anonymous memory and some pagecache
 * and check memory.current and some memory.stat values.
 */
static int test_memcg_current(const char *root)
{
	int ret = KSFT_FAIL;
	long current;
	char *memcg;

	memcg = cg_name(root, "memcg_test");
	if (!memcg)
		goto cleanup;

	if (cg_create(memcg))
		goto cleanup;

	current = cg_read_long(memcg, "memory.current");
	if (current != 0)
		goto cleanup;

	if (cg_run(memcg, alloc_anon_50M_check, NULL))
		goto cleanup;

	if (cg_run(memcg, alloc_pagecache_50M_check, NULL))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_destroy(memcg);
	free(memcg);

	return ret;
}

static int alloc_pagecache_50M(const char *cgroup, void *arg)
{
	int fd = (long)arg;

	return alloc_pagecache(fd, MB(50));
}

static int alloc_pagecache_50M_noexit(const char *cgroup, void *arg)
{
	int fd = (long)arg;
	int ppid = getppid();

	if (alloc_pagecache(fd, MB(50)))
		return -1;

	while (getppid() == ppid)
		sleep(1);

	return 0;
}

/*
 * First, this test creates the following hierarchy:
 * A       memory.min = 50M,  memory.max = 200M
 * A/B     memory.min = 50M,  memory.current = 50M
 * A/B/C   memory.min = 75M,  memory.current = 50M
 * A/B/D   memory.min = 25M,  memory.current = 50M
 * A/B/E   memory.min = 500M, memory.current = 0
 * A/B/F   memory.min = 0,    memory.current = 50M
 *
 * Usages are pagecache, but the test keeps a running
 * process in every leaf cgroup.
 * Then it creates A/G and creates a significant
 * memory pressure in it.
 *
 * A/B    memory.current ~= 50M
 * A/B/C  memory.current ~= 33M
 * A/B/D  memory.current ~= 17M
 * A/B/E  memory.current ~= 0
 *
 * After that it tries to allocate more than there is
 * unprotected memory in A available, and checks
 * checks that memory.min protects pagecache even
 * in this case.
 */
static int test_memcg_min(const char *root)
{
	int ret = KSFT_FAIL;
	char *parent[3] = {NULL};
	char *children[4] = {NULL};
	long c[4];
	int i, attempts;
	int fd;

	fd = get_temp_fd();
	if (fd < 0)
		goto cleanup;

	parent[0] = cg_name(root, "memcg_test_0");
	if (!parent[0])
		goto cleanup;

	parent[1] = cg_name(parent[0], "memcg_test_1");
	if (!parent[1])
		goto cleanup;

	parent[2] = cg_name(parent[0], "memcg_test_2");
	if (!parent[2])
		goto cleanup;

	if (cg_create(parent[0]))
		goto cleanup;

	if (cg_read_long(parent[0], "memory.min")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	if (cg_write(parent[0], "cgroup.subtree_control", "+memory"))
		goto cleanup;

	if (cg_write(parent[0], "memory.max", "200M"))
		goto cleanup;

	if (cg_write(parent[0], "memory.swap.max", "0"))
		goto cleanup;

	if (cg_create(parent[1]))
		goto cleanup;

	if (cg_write(parent[1], "cgroup.subtree_control", "+memory"))
		goto cleanup;

	if (cg_create(parent[2]))
		goto cleanup;

	for (i = 0; i < ARRAY_SIZE(children); i++) {
		children[i] = cg_name_indexed(parent[1], "child_memcg", i);
		if (!children[i])
			goto cleanup;

		if (cg_create(children[i]))
			goto cleanup;

		if (i == 2)
			continue;

		cg_run_nowait(children[i], alloc_pagecache_50M_noexit,
			      (void *)(long)fd);
	}

	if (cg_write(parent[0], "memory.min", "50M"))
		goto cleanup;
	if (cg_write(parent[1], "memory.min", "50M"))
		goto cleanup;
	if (cg_write(children[0], "memory.min", "75M"))
		goto cleanup;
	if (cg_write(children[1], "memory.min", "25M"))
		goto cleanup;
	if (cg_write(children[2], "memory.min", "500M"))
		goto cleanup;
	if (cg_write(children[3], "memory.min", "0"))
		goto cleanup;

	attempts = 0;
	while (!values_close(cg_read_long(parent[1], "memory.current"),
			     MB(150), 3)) {
		if (attempts++ > 5)
			break;
		sleep(1);
	}

	if (cg_run(parent[2], alloc_anon, (void *)MB(148)))
		goto cleanup;

	if (!values_close(cg_read_long(parent[1], "memory.current"), MB(50), 3))
		goto cleanup;

	for (i = 0; i < ARRAY_SIZE(children); i++)
		c[i] = cg_read_long(children[i], "memory.current");

	if (!values_close(c[0], MB(33), 10))
		goto cleanup;

	if (!values_close(c[1], MB(17), 10))
		goto cleanup;

	if (!values_close(c[2], 0, 1))
		goto cleanup;

	if (!cg_run(parent[2], alloc_anon, (void *)MB(170)))
		goto cleanup;

	if (!values_close(cg_read_long(parent[1], "memory.current"), MB(50), 3))
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	for (i = ARRAY_SIZE(children) - 1; i >= 0; i--) {
		if (!children[i])
			continue;

		cg_destroy(children[i]);
		free(children[i]);
	}

	for (i = ARRAY_SIZE(parent) - 1; i >= 0; i--) {
		if (!parent[i])
			continue;

		cg_destroy(parent[i]);
		free(parent[i]);
	}
	close(fd);
	return ret;
}

/*
 * First, this test creates the following hierarchy:
 * A       memory.low = 50M,  memory.max = 200M
 * A/B     memory.low = 50M,  memory.current = 50M
 * A/B/C   memory.low = 75M,  memory.current = 50M
 * A/B/D   memory.low = 25M,  memory.current = 50M
 * A/B/E   memory.low = 500M, memory.current = 0
 * A/B/F   memory.low = 0,    memory.current = 50M
 *
 * Usages are pagecache.
 * Then it creates A/G an creates a significant
 * memory pressure in it.
 *
 * Then it checks actual memory usages and expects that:
 * A/B    memory.current ~= 50M
 * A/B/   memory.current ~= 33M
 * A/B/D  memory.current ~= 17M
 * A/B/E  memory.current ~= 0
 *
 * After that it tries to allocate more than there is
 * unprotected memory in A available,
 * and checks low and oom events in memory.events.
 */
static int test_memcg_low(const char *root)
{
	int ret = KSFT_FAIL;
	char *parent[3] = {NULL};
	char *children[4] = {NULL};
	long low, oom;
	long c[4];
	int i;
	int fd;

	fd = get_temp_fd();
	if (fd < 0)
		goto cleanup;

	parent[0] = cg_name(root, "memcg_test_0");
	if (!parent[0])
		goto cleanup;

	parent[1] = cg_name(parent[0], "memcg_test_1");
	if (!parent[1])
		goto cleanup;

	parent[2] = cg_name(parent[0], "memcg_test_2");
	if (!parent[2])
		goto cleanup;

	if (cg_create(parent[0]))
		goto cleanup;

	if (cg_read_long(parent[0], "memory.low"))
		goto cleanup;

	if (cg_write(parent[0], "cgroup.subtree_control", "+memory"))
		goto cleanup;

	if (cg_write(parent[0], "memory.max", "200M"))
		goto cleanup;

	if (cg_write(parent[0], "memory.swap.max", "0"))
		goto cleanup;

	if (cg_create(parent[1]))
		goto cleanup;

	if (cg_write(parent[1], "cgroup.subtree_control", "+memory"))
		goto cleanup;

	if (cg_create(parent[2]))
		goto cleanup;

	for (i = 0; i < ARRAY_SIZE(children); i++) {
		children[i] = cg_name_indexed(parent[1], "child_memcg", i);
		if (!children[i])
			goto cleanup;

		if (cg_create(children[i]))
			goto cleanup;

		if (i == 2)
			continue;

		if (cg_run(children[i], alloc_pagecache_50M, (void *)(long)fd))
			goto cleanup;
	}

	if (cg_write(parent[0], "memory.low", "50M"))
		goto cleanup;
	if (cg_write(parent[1], "memory.low", "50M"))
		goto cleanup;
	if (cg_write(children[0], "memory.low", "75M"))
		goto cleanup;
	if (cg_write(children[1], "memory.low", "25M"))
		goto cleanup;
	if (cg_write(children[2], "memory.low", "500M"))
		goto cleanup;
	if (cg_write(children[3], "memory.low", "0"))
		goto cleanup;

	if (cg_run(parent[2], alloc_anon, (void *)MB(148)))
		goto cleanup;

	if (!values_close(cg_read_long(parent[1], "memory.current"), MB(50), 3))
		goto cleanup;

	for (i = 0; i < ARRAY_SIZE(children); i++)
		c[i] = cg_read_long(children[i], "memory.current");

	if (!values_close(c[0], MB(33), 10))
		goto cleanup;

	if (!values_close(c[1], MB(17), 10))
		goto cleanup;

	if (!values_close(c[2], 0, 1))
		goto cleanup;

	if (cg_run(parent[2], alloc_anon, (void *)MB(166))) {
		fprintf(stderr,
			"memory.low prevents from allocating anon memory\n");
		goto cleanup;
	}

	for (i = 0; i < ARRAY_SIZE(children); i++) {
		oom = cg_read_key_long(children[i], "memory.events", "oom ");
		low = cg_read_key_long(children[i], "memory.events", "low ");

		if (oom)
			goto cleanup;
		if (i < 2 && low <= 0)
			goto cleanup;
		if (i >= 2 && low)
			goto cleanup;
	}

	ret = KSFT_PASS;

cleanup:
	for (i = ARRAY_SIZE(children) - 1; i >= 0; i--) {
		if (!children[i])
			continue;

		cg_destroy(children[i]);
		free(children[i]);
	}

	for (i = ARRAY_SIZE(parent) - 1; i >= 0; i--) {
		if (!parent[i])
			continue;

		cg_destroy(parent[i]);
		free(parent[i]);
	}
	close(fd);
	return ret;
}

static int alloc_pagecache_max_30M(const char *cgroup, void *arg)
{
	size_t size = MB(50);
	int ret = -1;
	long current;
	int fd;

	fd = get_temp_fd();
	if (fd < 0)
		return -1;

	if (alloc_pagecache(fd, size))
		goto cleanup;

	current = cg_read_long(cgroup, "memory.current");
	if (current <= MB(29) || current > MB(30))
		goto cleanup;

	ret = 0;

cleanup:
	close(fd);
	return ret;

}

/*
 * This test checks that memory.high limits the amount of
 * memory which can be consumed by either anonymous memory
 * or pagecache.
 */
static int test_memcg_high(const char *root)
{
	int ret = KSFT_FAIL;
	char *memcg;
	long high;

	memcg = cg_name(root, "memcg_test");
	if (!memcg)
		goto cleanup;

	if (cg_create(memcg))
		goto cleanup;

	if (cg_read_strcmp(memcg, "memory.high", "max\n"))
		goto cleanup;

	if (cg_write(memcg, "memory.swap.max", "0"))
		goto cleanup;

	if (cg_write(memcg, "memory.high", "30M"))
		goto cleanup;

	if (cg_run(memcg, alloc_anon, (void *)MB(100)))
		goto cleanup;

	if (!cg_run(memcg, alloc_pagecache_50M_check, NULL))
		goto cleanup;

	if (cg_run(memcg, alloc_pagecache_max_30M, NULL))
		goto cleanup;

	high = cg_read_key_long(memcg, "memory.events", "high ");
	if (high <= 0)
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_destroy(memcg);
	free(memcg);

	return ret;
}

/*
 * This test checks that memory.max limits the amount of
 * memory which can be consumed by either anonymous memory
 * or pagecache.
 */
static int test_memcg_max(const char *root)
{
	int ret = KSFT_FAIL;
	char *memcg;
	long current, max;

	memcg = cg_name(root, "memcg_test");
	if (!memcg)
		goto cleanup;

	if (cg_create(memcg))
		goto cleanup;

	if (cg_read_strcmp(memcg, "memory.max", "max\n"))
		goto cleanup;

	if (cg_write(memcg, "memory.swap.max", "0"))
		goto cleanup;

	if (cg_write(memcg, "memory.max", "30M"))
		goto cleanup;

	/* Should be killed by OOM killer */
	if (!cg_run(memcg, alloc_anon, (void *)MB(100)))
		goto cleanup;

	if (cg_run(memcg, alloc_pagecache_max_30M, NULL))
		goto cleanup;

	current = cg_read_long(memcg, "memory.current");
	if (current > MB(30) || !current)
		goto cleanup;

	max = cg_read_key_long(memcg, "memory.events", "max ");
	if (max <= 0)
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_destroy(memcg);
	free(memcg);

	return ret;
}

static int alloc_anon_50M_check_swap(const char *cgroup, void *arg)
{
	long mem_max = (long)arg;
	size_t size = MB(50);
	char *buf, *ptr;
	long mem_current, swap_current;
	int ret = -1;

	buf = malloc(size);
	for (ptr = buf; ptr < buf + size; ptr += PAGE_SIZE)
		*ptr = 0;

	mem_current = cg_read_long(cgroup, "memory.current");
	if (!mem_current || !values_close(mem_current, mem_max, 3))
		goto cleanup;

	swap_current = cg_read_long(cgroup, "memory.swap.current");
	if (!swap_current ||
	    !values_close(mem_current + swap_current, size, 3))
		goto cleanup;

	ret = 0;
cleanup:
	free(buf);
	return ret;
}

/*
 * This test checks that memory.swap.max limits the amount of
 * anonymous memory which can be swapped out.
 */
static int test_memcg_swap_max(const char *root)
{
	int ret = KSFT_FAIL;
	char *memcg;
	long max;

	if (!is_swap_enabled())
		return KSFT_SKIP;

	memcg = cg_name(root, "memcg_test");
	if (!memcg)
		goto cleanup;

	if (cg_create(memcg))
		goto cleanup;

	if (cg_read_long(memcg, "memory.swap.current")) {
		ret = KSFT_SKIP;
		goto cleanup;
	}

	if (cg_read_strcmp(memcg, "memory.max", "max\n"))
		goto cleanup;

	if (cg_read_strcmp(memcg, "memory.swap.max", "max\n"))
		goto cleanup;

	if (cg_write(memcg, "memory.swap.max", "30M"))
		goto cleanup;

	if (cg_write(memcg, "memory.max", "30M"))
		goto cleanup;

	/* Should be killed by OOM killer */
	if (!cg_run(memcg, alloc_anon, (void *)MB(100)))
		goto cleanup;

	if (cg_read_key_long(memcg, "memory.events", "oom ") != 1)
		goto cleanup;

	if (cg_read_key_long(memcg, "memory.events", "oom_kill ") != 1)
		goto cleanup;

	if (cg_run(memcg, alloc_anon_50M_check_swap, (void *)MB(30)))
		goto cleanup;

	max = cg_read_key_long(memcg, "memory.events", "max ");
	if (max <= 0)
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_destroy(memcg);
	free(memcg);

	return ret;
}

/*
 * This test disables swapping and tries to allocate anonymous memory
 * up to OOM. Then it checks for oom and oom_kill events in
 * memory.events.
 */
static int test_memcg_oom_events(const char *root)
{
	int ret = KSFT_FAIL;
	char *memcg;

	memcg = cg_name(root, "memcg_test");
	if (!memcg)
		goto cleanup;

	if (cg_create(memcg))
		goto cleanup;

	if (cg_write(memcg, "memory.max", "30M"))
		goto cleanup;

	if (cg_write(memcg, "memory.swap.max", "0"))
		goto cleanup;

	if (!cg_run(memcg, alloc_anon, (void *)MB(100)))
		goto cleanup;

	if (cg_read_strcmp(memcg, "cgroup.procs", ""))
		goto cleanup;

	if (cg_read_key_long(memcg, "memory.events", "oom ") != 1)
		goto cleanup;

	if (cg_read_key_long(memcg, "memory.events", "oom_kill ") != 1)
		goto cleanup;

	ret = KSFT_PASS;

cleanup:
	cg_destroy(memcg);
	free(memcg);

	return ret;
}

#define T(x) { x, #x }
struct memcg_test {
	int (*fn)(const char *root);
	const char *name;
} tests[] = {
	T(test_memcg_subtree_control),
	T(test_memcg_current),
	T(test_memcg_min),
	T(test_memcg_low),
	T(test_memcg_high),
	T(test_memcg_max),
	T(test_memcg_oom_events),
	T(test_memcg_swap_max),
};
#undef T

int main(int argc, char **argv)
{
	char root[PATH_MAX];
	int i, ret = EXIT_SUCCESS;

	if (cg_find_unified_root(root, sizeof(root)))
		ksft_exit_skip("cgroup v2 isn't mounted\n");

	/*
	 * Check that memory controller is available:
	 * memory is listed in cgroup.controllers
	 */
	if (cg_read_strstr(root, "cgroup.controllers", "memory"))
		ksft_exit_skip("memory controller isn't available\n");

	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		switch (tests[i].fn(root)) {
		case KSFT_PASS:
			ksft_test_result_pass("%s\n", tests[i].name);
			break;
		case KSFT_SKIP:
			ksft_test_result_skip("%s\n", tests[i].name);
			break;
		default:
			ret = EXIT_FAILURE;
			ksft_test_result_fail("%s\n", tests[i].name);
			break;
		}
	}

	return ret;
}
