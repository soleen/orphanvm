// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

#include "ovm_workload.h"

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s -c <cpu_id>\n", prog);
	exit(1);
}

static uint64_t get_page_gpa(void *addr)
{
	uintptr_t vaddr = (uintptr_t)addr;
	uint64_t entry = 0;
	uint64_t pfn = 0;
	int fd;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		return 0;

	off_t offset = (off_t)((vaddr / 4096) * sizeof(uint64_t));
	if (pread(fd, &entry, sizeof(entry), offset) == sizeof(entry)) {
		/* Bit 63 indicates whether page is present in RAM */
		if (entry & (1ULL << 63))
			pfn = entry & ((1ULL << 55) - 1);
	}
	close(fd);

	if (!pfn)
		return 0;

	return (pfn << 12) | (vaddr & 0xfff);
}

int main(int argc, char **argv)
{
	volatile uint64_t *words;
	cpu_set_t cpuset;
	int cpu_specified = 0;
	int cpu_id = -1;
	uint64_t pass = 0;
	size_t i;
	void *mem;
	int opt;

	while ((opt = getopt(argc, argv, "c:h")) != -1) {
		switch (opt) {
		case 'c': {
			char *endptr;
			long val = strtol(optarg, &endptr, 10);
			if (*endptr != '\0' || val < 0 || val >= CPU_SETSIZE)
				errx(1, "Invalid CPU id '%s'", optarg);
			cpu_id = (int)val;
			cpu_specified = 1;
			break;
		}
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	if (!cpu_specified || cpu_id < 0)
		errx(1, "Error: -c <cpu_id> is required.");

	/* Set real-time SCHED_FIFO scheduling priority */
	struct sched_param sp = { .sched_priority = 99 };
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		setpriority(PRIO_PROCESS, 0, -20);

	/* Bind process to target CPU core */
	CPU_ZERO(&cpuset);
	CPU_SET(cpu_id, &cpuset);
	if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0)
		err(1, "sched_setaffinity to CPU %d failed", cpu_id);

	/* 1. Reserve 2MB HugeTLB page (quit with failure if fails) */
	mem = mmap(NULL, OVM_HUGEPAGE_2MB_SIZE,
		   PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
		   -1, 0);

	if (mem == MAP_FAILED)
		err(1, "Failed to allocate 2MB HugeTLB page");

	/* 2. Lock page in physical RAM (quit with failure if fails) */
	if (mlock(mem, OVM_HUGEPAGE_2MB_SIZE) < 0)
		err(1, "mlock 2MB HugeTLB page failed");

	words = (volatile uint64_t *)mem;

	/* 3. Set the first 128-bit to super unique magic value with cpu_id */
	words[0] = OVM_MAGIC0;
	words[1] = OVM_MAGIC1(cpu_id);
	__sync_synchronize();

	/* 4. Query GPA from /proc/self/pagemap and announce on console */
	uint64_t gpa = get_page_gpa(mem);
	if (gpa) {
		int kfd = open("/dev/kmsg", O_WRONLY);
		if (kfd >= 0) {
			dprintf(kfd, "<0>[OVM_AGENT] cpu=%d gpa=0x%llx\n",
				cpu_id, (unsigned long long)gpa);
			close(kfd);
		}
		printf("[OVM_AGENT] cpu=%d gpa=0x%llx\n", cpu_id,
		       (unsigned long long)gpa);
		fflush(stdout);
	}

	/* 5. Continuous 2MB memory pass loop */
	for (;;) {
		for (i = OVM_DATA_START_IDX; i < OVM_TOTAL_WORDS; i++)
			words[i] = pass;
#if defined(__aarch64__)
		asm volatile("dc cvac, %0\n dsb ish" :: "r"(&words[OVM_DATA_START_IDX]) : "memory");
#elif defined(__x86_64__)
		asm volatile("clflush (%0)\n mfence" :: "r"(&words[OVM_DATA_START_IDX]) : "memory");
#endif
		pass++;
	}

	return 0;
}
