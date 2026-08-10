// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#include "ovm_workload.h"

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

#ifndef MEMBARRIER_CMD_QUERY
#define MEMBARRIER_CMD_QUERY 0
#endif
#ifndef MEMBARRIER_CMD_PRIVATE_EXPEDITED
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED (1 << 3)
#endif
#ifndef MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED (1 << 4)
#endif

#ifndef SYS_membarrier
#if defined(__x86_64__)
#define SYS_membarrier 324
#elif defined(__aarch64__)
#define SYS_membarrier 283
#endif
#endif

#define MAX_CPUS 256
#define IPI_INTERVAL 50

static inline void cpu_relax(void)
{
#if defined(__x86_64__)
	asm volatile("rep; nop" ::: "memory");
#elif defined(__aarch64__)
	asm volatile("yield" ::: "memory");
#else
	sched_yield();
#endif
}

struct cpu_worker {
	int cpu_id;
	volatile uint64_t *words;
	void *shootdown_page;
	pthread_t thread;
	uint64_t pass;
};

static volatile int running = 1;
static int membarrier_supported = 0;
static struct cpu_worker workers[MAX_CPUS];

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
		if (entry & (1ULL << 63))
			pfn = entry & ((1ULL << 55) - 1);
	}
	close(fd);

	if (!pfn)
		return 0;

	return (pfn << 12) | (vaddr & 0xfff);
}

static void trigger_cross_cpu_ipi(struct cpu_worker *w)
{
	w->words[3] = 2; /* before membarrier */
#if defined(__x86_64__)
	asm volatile("clflush (%0)\n mfence" :: "r"(&w->words[3]) : "memory");
#endif
#ifdef SYS_membarrier
	if (membarrier_supported)
		syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0);
#endif
	w->words[3] = 3; /* after membarrier, before madvise */
#if defined(__x86_64__)
	asm volatile("clflush (%0)\n mfence" :: "r"(&w->words[3]) : "memory");
#endif

	if (w->shootdown_page && w->shootdown_page != MAP_FAILED) {
		*(volatile char *)w->shootdown_page = (char)(w->pass & 0xff);
		madvise(w->shootdown_page, 4096, MADV_DONTNEED);
	}
	w->words[3] = 4; /* after madvise */
	w->words[4]++;   /* completed IPI trigger count */
#if defined(__x86_64__)
	asm volatile("clflush (%0)\n clflush (%1)\n mfence" :: "r"(&w->words[3]), "r"(&w->words[4]) : "memory");
#endif
}

static void *worker_thread_fn(void *arg)
{
	struct cpu_worker *w = (struct cpu_worker *)arg;
	cpu_set_t cpuset;

	CPU_ZERO(&cpuset);
	CPU_SET(w->cpu_id, &cpuset);
	if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) < 0)
		warn("pthread_setaffinity_np to CPU %d failed", w->cpu_id);

	while (running) {
		w->words[3] = 1; /* running workload loop */
		w->words[OVM_DATA_START_IDX] = ++w->pass;
#if defined(__aarch64__)
		asm volatile("dc cvac, %0\n dsb ish" :: "r"(&w->words[OVM_DATA_START_IDX]) : "memory");
#elif defined(__x86_64__)
		asm volatile("clflush (%0)\n mfence" :: "r"(&w->words[OVM_DATA_START_IDX]) : "memory");
#endif

		/* Periodically trigger cross-CPU shootdown / IPI */
		if ((w->pass % IPI_INTERVAL) == 0) {
			trigger_cross_cpu_ipi(w);

			if (w->cpu_id == 0 && (w->pass % 5000) == 0) {
				int kfd = open("/dev/kmsg", O_WRONLY);
				if (kfd >= 0) {
					dprintf(kfd, "<6>[OVM_IPI] cpu=0 pass=%llu\n",
						(unsigned long long)w->pass);
					close(kfd);
				}
			}
		}

		for (volatile int j = 0; j < 50; j++)
			cpu_relax();
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int ncpus;

	(void)argc;
	(void)argv;

	ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (ncpus < 1)
		ncpus = 1;
	if (ncpus > MAX_CPUS)
		ncpus = MAX_CPUS;

	printf("[OVM_IPI] Initializing multi-threaded cross-vCPU IPI test for %d CPUs...\n", ncpus);
	fflush(stdout);

#ifdef SYS_membarrier
	if (syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0, 0) == 0)
		membarrier_supported = 1;
#endif

	/* Allocate 2MB HugeTLB page & private shootdown page for each CPU */
	for (int i = 0; i < ncpus; i++) {
		workers[i].cpu_id = i;
		workers[i].pass = 0;

		void *mem = mmap(NULL, OVM_HUGEPAGE_2MB_SIZE,
				 PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
				 -1, 0);
		if (mem == MAP_FAILED)
			err(1, "Failed to allocate 2MB HugeTLB page for CPU %d", i);

		if (mlock(mem, OVM_HUGEPAGE_2MB_SIZE) < 0)
			warn("mlock 2MB HugeTLB page for CPU %d failed", i);

		workers[i].words = (volatile uint64_t *)mem;
		workers[i].words[0] = OVM_MAGIC0;
		workers[i].words[1] = OVM_MAGIC1(i);
		__sync_synchronize();

		workers[i].shootdown_page = mmap(NULL, 4096,
						 PROT_READ | PROT_WRITE,
						 MAP_PRIVATE | MAP_ANONYMOUS,
						 -1, 0);

		uint64_t gpa = get_page_gpa(mem);
		if (gpa) {
			int kfd = open("/dev/kmsg", O_WRONLY);
			if (kfd >= 0) {
				dprintf(kfd, "<0>[OVM_AGENT] cpu=%d gpa=0x%llx\n",
					i, (unsigned long long)gpa);
				close(kfd);
			}
			printf("[OVM_AGENT] cpu=%d gpa=0x%llx\n", i,
			       (unsigned long long)gpa);
			fflush(stdout);
		}
	}

	/* Spawn worker threads for secondary CPUs */
	for (int i = 1; i < ncpus; i++) {
		if (pthread_create(&workers[i].thread, NULL, worker_thread_fn, &workers[i]) < 0)
			warn("pthread_create for CPU %d failed", i);
	}

	printf("[OVM_IPI] Cross-vCPU IPI loop active across %d CPUs (membarrier=%s)...\n",
	       ncpus, membarrier_supported ? "yes" : "no");
	fflush(stdout);

	/* Main process thread executes worker 0 pinned to CPU 0 */
	worker_thread_fn(&workers[0]);

	running = 0;
	return 0;
}
