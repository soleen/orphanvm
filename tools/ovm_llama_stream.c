// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ovm_llama_stream.h"

#define OVM_PAGE_2MB_SIZE (2 * 1024 * 1024)

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

static struct ovm_llama_stream *g_stream = NULL;

static uint64_t get_time_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
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
		if (entry & (1ULL << 63))
			pfn = entry & ((1ULL << 55) - 1);
	}
	close(fd);

	if (!pfn)
		return 0;

	return (pfn << 12) | (vaddr & 0xfff);
}

static void flush_cache(const void *addr)
{
	(void)addr;
#if defined(__x86_64__)
	asm volatile("clflush (%0)\n mfence" :: "r"(addr) : "memory");
#elif defined(__aarch64__)
	asm volatile("dc cvac, %0\n dsb ish" :: "r"(addr) : "memory");
#endif
}

void ovm_llama_stream_init(void)
{
	void *mem;
	uint64_t gpa;
	int kfd;

	if (g_stream)
		return;

	/* 1. Try 2MB HugeTLB first */
	mem = mmap(NULL, OVM_PAGE_2MB_SIZE,
		   PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB,
		   -1, 0);
	if (mem == MAP_FAILED) {
		/* Fallback to standard anonymous 2MB page */
		mem = mmap(NULL, OVM_PAGE_2MB_SIZE,
			   PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS,
			   -1, 0);
	}

	if (mem == MAP_FAILED) {
		fprintf(stderr, "[OVM_LLAMA] Warning: Failed to allocate shared memory stream\n");
		return;
	}

	(void)mlock(mem, OVM_PAGE_2MB_SIZE);
	memset(mem, 0, sizeof(struct ovm_llama_stream));

	g_stream = (struct ovm_llama_stream *)mem;
	g_stream->magic = OVM_LLAMA_MAGIC;
	g_stream->version = OVM_LLAMA_VERSION;
	g_stream->status = OVM_LLAMA_STATUS_LOADING;
	g_stream->start_time_ms = get_time_ms();
	g_stream->last_time_ms = g_stream->start_time_ms;

	gpa = get_page_gpa(mem);
	g_stream->gpa = gpa;

	__sync_synchronize();
	flush_cache(&g_stream->magic);
	flush_cache(&g_stream->status);
	flush_cache(&g_stream->gpa);

	/* Announce GPA for host discovery */
	kfd = open("/dev/kmsg", O_WRONLY);
	if (kfd >= 0) {
		dprintf(kfd, "<0>[OVM_LLAMA] gpa=0x%llx size=0x%lx\n",
			(unsigned long long)gpa, (unsigned long)sizeof(struct ovm_llama_stream));
		close(kfd);
	}
	printf("[OVM_LLAMA] gpa=0x%llx size=0x%lx\n",
	       (unsigned long long)gpa, (unsigned long)sizeof(struct ovm_llama_stream));
	fflush(stdout);
}

void ovm_llama_stream_set_status(uint32_t status)
{
	if (!g_stream)
		return;

	g_stream->status = status;
	g_stream->last_time_ms = get_time_ms();
	__sync_synchronize();
	flush_cache(&g_stream->status);
}

void ovm_llama_stream_set_prompt_tokens(uint64_t count)
{
	if (!g_stream)
		return;

	g_stream->prompt_tokens = count;
	__sync_synchronize();
	flush_cache(&g_stream->prompt_tokens);
}

void ovm_llama_stream_append(const char *piece, size_t len)
{
	if (!g_stream || !piece || len == 0)
		return;

	if (g_stream->status != OVM_LLAMA_STATUS_GENERATING) {
		g_stream->status = OVM_LLAMA_STATUS_GENERATING;
		flush_cache(&g_stream->status);
	}

	for (size_t i = 0; i < len; i++) {
		g_stream->buffer[g_stream->head] = piece[i];
		flush_cache(&g_stream->buffer[g_stream->head]);
		g_stream->head = (g_stream->head + 1) % OVM_LLAMA_BUF_SIZE;
		g_stream->total_chars++;
	}

	g_stream->total_tokens++;
	g_stream->last_time_ms = get_time_ms();
	__sync_synchronize();
	flush_cache(&g_stream->total_tokens);
	flush_cache(&g_stream->total_chars);
	flush_cache(&g_stream->head);
	flush_cache(&g_stream->last_time_ms);
}

void ovm_llama_stream_finish(void)
{
	if (!g_stream)
		return;

	g_stream->status = OVM_LLAMA_STATUS_FINISHED;
	g_stream->last_time_ms = get_time_ms();
	__sync_synchronize();
	flush_cache(&g_stream->status);
	flush_cache(&g_stream->last_time_ms);
}

struct ovm_llama_stream *ovm_llama_stream_get(void)
{
	return g_stream;
}
