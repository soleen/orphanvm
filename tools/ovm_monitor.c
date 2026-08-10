// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ovm_workload.h"
#include "ovm_llama_stream.h"

#define MAX_CPUS 256
#define MAX_MEM_REGIONS 16

static int verbose_mode = 0;

struct nvmm_mem_region {
	uint32_t slot;
	uint64_t gpa;
	uint64_t hva;
	uint64_t size;
};

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s -F [-a] [-c <cpu_id>] [-f <file>] [-v]\n",
		prog);
	fprintf(stderr,
		"       %s -O <offset[,offset...]> [-c <cpu_id>] [-f <file>]\n",
		prog);
	fprintf(stderr,
		"       %s -L [-f <file>] [-v]\n",
		prog);
	fprintf(stderr,
		"       %s -S <offset> [-p <pos>] [-w <ms>] [-r] [-f <file>]\n",
		prog);
	fprintf(stderr, "Modes:\n");
	fprintf(stderr,
		"  -F             Find guest agent 2MB page offset via pagemap\n");
	fprintf(stderr,
		"  -O <offset>    Read loop number from offset(s)\n");
	fprintf(stderr,
		"  -L             Find LLAMA stream buffer offset via pagemap\n");
	fprintf(stderr,
		"  -S <offset>    Read LLAMA stream from offset\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr,
		"  -a             Find all active CPUs in single pass\n");
	fprintf(stderr,
		"  -c <cpu_id>    Target guest CPU core ID\n");
	fprintf(stderr,
		"  -p <pos>       Start character position for -S (default: 0)\n");
	fprintf(stderr,
		"  -w <ms>        Poll interval in ms for continuous follow streaming\n");
	fprintf(stderr,
		"  -r             Raw text output mode for -S (no metadata header)\n");
	fprintf(stderr,
		"  -f <path>      Target memory file (default: /dev/mem)\n");
	fprintf(stderr,
		"  -v             Verbose debug output with timing\n");
	fprintf(stderr,
		"  -H             Header only mode for -S (metadata only, do not dump text)\n");
	fprintf(stderr,
		"  -h             Show this help message\n");
	exit(1);
}

static uint64_t get_time_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int is_regular_file(int fd)
{
	struct stat st;

	return (fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
}

static pid_t find_nvmm_pid(void)
{
	const char *pid_files[] = {
		"/tmp/nvmm.pid",
		"/tmp/ovm/nvmm.pid",
		"/tmp/guest_monitor.pid",
		"/var/run/nvmm.pid",
	};
	char path[512], comm[64];
	DIR *dir;
	struct dirent *entry;
	FILE *fp;
	size_t i;

	/* 1. Fast check via PID files */
	for (i = 0; i < sizeof(pid_files) / sizeof(pid_files[0]); i++) {
		fp = fopen(pid_files[i], "r");
		if (fp) {
			long pid_val = -1;
			if (fscanf(fp, "%ld", &pid_val) == 1 && pid_val > 0) {
				fclose(fp);
				snprintf(path, sizeof(path), "/proc/%ld/comm", pid_val);
				fp = fopen(path, "r");
				if (fp) {
					if (fgets(comm, sizeof(comm), fp)) {
						char *nl = strchr(comm, '\n');
						if (nl)
							*nl = '\0';
						if (strstr(comm, "nvmm") || strstr(comm, "nanovmm")) {
							fclose(fp);
							return (pid_t)pid_val;
						}
					}
					fclose(fp);
				}
			} else {
				fclose(fp);
			}
		}
	}

	/* 2. Scan /proc for nvmm process */
	dir = opendir("/proc");
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
			continue;

		snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
		fp = fopen(path, "r");
		if (!fp)
			continue;

		if (fgets(comm, sizeof(comm), fp)) {
			char *nl = strchr(comm, '\n');
			if (nl)
				*nl = '\0';
			if (strcmp(comm, "nvmm") == 0 || strcmp(comm, "nanovmm") == 0) {
				pid_t found_pid = (pid_t)atoi(entry->d_name);
				fclose(fp);
				closedir(dir);
				return found_pid;
			}
		}
		fclose(fp);
	}
	closedir(dir);
	return -1;
}

static void parse_log_file(const char *log_path,
			   struct nvmm_mem_region *regions,
			   int max_regions, int *region_count,
			   uint64_t *agent_gpas,
			   uint8_t *agent_gpa_present,
			   int max_cpus,
			   uint64_t *llama_gpa,
			   uint8_t *llama_gpa_present)
{
	FILE *fp = fopen(log_path, "r");
	char line[512];

	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char *p;

		if ((p = strstr(line, "[NVMM_MEM]"))) {
			uint32_t slot = 0;
			unsigned long long gpa = 0, hva = 0, size = 0;
			if (sscanf(p, "[NVMM_MEM] slot=%u gpa=%llx hva=%llx size=%llx",
				   &slot, &gpa, &hva, &size) == 4 ||
			    sscanf(p, "[NVMM_MEM] slot=%u gpa=0x%llx hva=0x%llx size=0x%llx",
				   &slot, &gpa, &hva, &size) == 4) {
				int exists = 0;
				for (int r = 0; r < *region_count; r++) {
					if (regions[r].slot == slot &&
					    regions[r].gpa == gpa) {
						regions[r].hva = hva;
						regions[r].size = size;
						exists = 1;
						break;
					}
				}
				if (!exists && *region_count < max_regions) {
					regions[*region_count].slot = slot;
					regions[*region_count].gpa = gpa;
					regions[*region_count].hva = hva;
					regions[*region_count].size = size;
					(*region_count)++;
				}
			}
		} else {
			char *p = line;
			while ((p = strstr(p, "[OVM_AGENT]"))) {
				int cpu = -1;
				unsigned long long gpa = 0;
				int n = 0;
				if (sscanf(p, "[OVM_AGENT] cpu=%d gpa=0x%llx%n",
					   &cpu, &gpa, &n) >= 2 ||
				    sscanf(p, "[OVM_AGENT] cpu=%d gpa=%llx%n",
					   &cpu, &gpa, &n) >= 2) {
					char next_ch = p[n];
					if (next_ch == '\0' || isspace((unsigned char)next_ch)) {
						if (cpu >= 0 && cpu < max_cpus &&
						    gpa != 0 && (gpa & (OVM_HUGEPAGE_2MB_SIZE - 1)) == 0) {
							agent_gpas[cpu] = gpa;
							agent_gpa_present[cpu] = 1;
						}
					}
				}
				p++;
			}
			p = line;
			while ((p = strstr(p, "[OVM_LLAMA]"))) {
				unsigned long long gpa = 0, sz = 0;
				int n = 0;
				if (sscanf(p, "[OVM_LLAMA] gpa=0x%llx size=0x%llx%n",
					   &gpa, &sz, &n) >= 1 ||
				    sscanf(p, "[OVM_LLAMA] gpa=%llx size=%llx%n",
					   &gpa, &sz, &n) >= 1 ||
				    sscanf(p, "[OVM_LLAMA] gpa=0x%llx%n",
					   &gpa, &n) >= 1 ||
				    sscanf(p, "[OVM_LLAMA] gpa=%llx%n",
					   &gpa, &n) >= 1) {
					if (llama_gpa && gpa != 0) {
						*llama_gpa = gpa;
						if (llama_gpa_present)
							*llama_gpa_present = 1;
					}
				}
				p++;
			}
		}
	}
	fclose(fp);
}

static uint64_t translate_hva_to_hpa(pid_t pid, uint64_t hva)
{
	char proc_path[64];
	uint64_t entry = 0;
	uint64_t pfn = 0;
	uint64_t dummy;
	int fd;

	/*
	 * Touch the HVA via /proc/$pid/mem to ensure the page table entry
	 * is populated before querying pagemap.
	 */
	snprintf(proc_path, sizeof(proc_path), "/proc/%d/mem", (int)pid);
	fd = open(proc_path, O_RDONLY);
	if (fd >= 0) {
		(void)pread(fd, &dummy, sizeof(dummy), (off_t)hva);
		close(fd);
	}

	snprintf(proc_path, sizeof(proc_path), "/proc/%d/pagemap", (int)pid);
	fd = open(proc_path, O_RDONLY);
	if (fd < 0)
		return 0;

	off_t offset = (off_t)((hva / 4096) * sizeof(uint64_t));
	if (pread(fd, &entry, sizeof(entry), offset) == sizeof(entry)) {
		if (entry & (1ULL << 63))
			pfn = entry & ((1ULL << 55) - 1);
	}
	close(fd);

	if (!pfn)
		return 0;

	return (pfn << 12) | (hva & 0xfff);
}

static int verify_magic_at_hva(pid_t pid, uint64_t hva, int expected_cpu)
{
	char proc_path[64];
	uint64_t header[2];
	int fd;

	snprintf(proc_path, sizeof(proc_path), "/proc/%d/mem", (int)pid);
	fd = open(proc_path, O_RDONLY);
	if (fd < 0)
		return 0;

	ssize_t n = pread(fd, header, sizeof(header), (off_t)hva);
	close(fd);

	if (n != (ssize_t)sizeof(header))
		return 0;

	if (header[0] != OVM_MAGIC0 || !OVM_MAGIC1_MATCH(header[1]))
		return 0;

	int found_cpu = OVM_MAGIC1_GET_CPU(header[1]);
	if (expected_cpu >= 0 && found_cpu != expected_cpu)
		return 0;

	return 1;
}

static int verify_magic_at_hpa(int mem_fd, uint64_t hpa, int expected_cpu)
{
	uint64_t header[2];

	if (mem_fd < 0)
		return 0;

	if (pread(mem_fd, header, sizeof(header), (off_t)hpa) != (ssize_t)sizeof(header))
		return 0;

	if (header[0] != OVM_MAGIC0 || !OVM_MAGIC1_MATCH(header[1]))
		return 0;

	int found_cpu = OVM_MAGIC1_GET_CPU(header[1]);
	if (expected_cpu >= 0 && found_cpu != expected_cpu)
		return 0;

	return 1;
}

static int find_agents_via_pagemap(int mem_fd, int target_cpu,
				   uint64_t *offsets, int max_cpus,
				   int *found_count, uint64_t *single_offset)
{
	const char *log_paths[] = {
		"/tmp/nvmm.log",
		"/tmp/ovm/nvmm.log",
		"/tmp/guest_monitor.log",
		"/var/log/nvmm.log"
	};
	struct nvmm_mem_region regions[MAX_MEM_REGIONS];
	uint64_t agent_gpas[MAX_CPUS];
	uint8_t agent_gpa_present[MAX_CPUS];
	int num_regions = 0;
	pid_t nvmm_pid;
	int cpu;
	size_t i;

	memset(regions, 0, sizeof(regions));
	memset(agent_gpas, 0, sizeof(agent_gpas));
	memset(agent_gpa_present, 0, sizeof(agent_gpa_present));

	nvmm_pid = find_nvmm_pid();
	if (nvmm_pid < 0) {
		if (verbose_mode)
			fprintf(stderr, "find_via_pagemap: nvmm process not found in /proc\n");
		return 0;
	}

	for (i = 0; i < sizeof(log_paths) / sizeof(log_paths[0]); i++) {
		parse_log_file(log_paths[i], regions, MAX_MEM_REGIONS,
			       &num_regions, agent_gpas, agent_gpa_present,
			       max_cpus, NULL, NULL);
	}

	if (num_regions == 0) {
		if (verbose_mode)
			fprintf(stderr, "find_via_pagemap: no NVMM_MEM regions found\n");
		return 0;
	}

	for (cpu = 0; cpu < max_cpus; cpu++) {
		if (target_cpu >= 0 && cpu != target_cpu)
			continue;

		if (!agent_gpa_present[cpu])
			continue;

		uint64_t gpa = agent_gpas[cpu];
		uint64_t hva = 0;
		int r;

		for (r = 0; r < num_regions; r++) {
			if (gpa >= regions[r].gpa &&
			    gpa < regions[r].gpa + regions[r].size) {
				hva = regions[r].hva + (gpa - regions[r].gpa);
				break;
			}
		}

		if (!hva) {
			if (verbose_mode)
				fprintf(stderr, "find_via_pagemap: CPU %d GPA 0x%llx outside NVMM regions\n",
					cpu, (unsigned long long)gpa);
			continue;
		}

		if (!verify_magic_at_hva(nvmm_pid, hva, cpu)) {
			if (verbose_mode)
				fprintf(stderr, "find_via_pagemap: CPU %d HVA 0x%llx magic check failed\n",
					cpu, (unsigned long long)hva);
			continue;
		}

		uint64_t hpa = translate_hva_to_hpa(nvmm_pid, hva);
		if (!hpa) {
			if (verbose_mode)
				fprintf(stderr, "find_via_pagemap: CPU %d HVA 0x%llx pagemap lookup returned 0\n",
					cpu, (unsigned long long)hva);
			continue;
		}

		if (mem_fd >= 0 && !verify_magic_at_hpa(mem_fd, hpa, cpu)) {
			if (verbose_mode)
				fprintf(stderr,
					"find_via_pagemap: CPU %d HPA 0x%llx magic check failed on mem_fd (continuing with verified HVA)\n",
					cpu, (unsigned long long)hpa);
		}

		if (offsets && offsets[cpu] == 0) {
			offsets[cpu] = hpa;
			if (found_count)
				(*found_count)++;
		}
		if (single_offset && (target_cpu < 0 || target_cpu == cpu)) {
			*single_offset = hpa;
		}
		if (verbose_mode)
			fprintf(stderr,
				"find_via_pagemap: CPU %d GPA 0x%llx -> HVA 0x%llx -> HPA 0x%llx (VERIFIED)\n",
				cpu, (unsigned long long)gpa,
				(unsigned long long)hva,
				(unsigned long long)hpa);
	}

	if (single_offset && *single_offset != 0)
		return 1;

	if (found_count && *found_count > 0) {
		if (target_cpu >= 0 && offsets && offsets[target_cpu] != 0)
			return 1;
		if (target_cpu < 0 && *found_count >= 1)
			return 1;
	}

	return 0;
}

static int verify_llama_magic_at_hva(pid_t pid, uint64_t hva)
{
	char proc_path[64];
	uint64_t magic = 0;
	int fd;

	snprintf(proc_path, sizeof(proc_path), "/proc/%d/mem", (int)pid);
	fd = open(proc_path, O_RDONLY);
	if (fd < 0)
		return 0;

	if (pread(fd, &magic, sizeof(magic), (off_t)hva) == (ssize_t)sizeof(magic)) {
		close(fd);
		return (magic == OVM_LLAMA_MAGIC);
	}
	close(fd);
	return 0;
}

static int verify_llama_magic_at_hpa(int mem_fd, uint64_t hpa)
{
	uint64_t magic = 0;

	if (mem_fd < 0)
		return 0;

	if (pread(mem_fd, &magic, sizeof(magic), (off_t)hpa) == (ssize_t)sizeof(magic))
		return (magic == OVM_LLAMA_MAGIC);

	return 0;
}

static void resolve_stream_page_hpas(int mem_fd, uint64_t base_hpa, uint64_t *page_hpas);

static int find_llama_via_pagemap(int mem_fd, uint64_t *discovered_offset)
{
	const char *log_paths[] = {
		"/tmp/nvmm.log",
		"/tmp/ovm/nvmm.log",
		"/tmp/guest_monitor.log",
		"/var/log/nvmm.log"
	};
	struct nvmm_mem_region regions[MAX_MEM_REGIONS];
	uint64_t agent_gpas[MAX_CPUS];
	uint8_t agent_gpa_present[MAX_CPUS];
	uint64_t llama_gpa = 0;
	uint8_t llama_gpa_present = 0;
	int num_regions = 0;
	pid_t nvmm_pid;
	size_t i;

	memset(regions, 0, sizeof(regions));
	memset(agent_gpas, 0, sizeof(agent_gpas));
	memset(agent_gpa_present, 0, sizeof(agent_gpa_present));

	nvmm_pid = find_nvmm_pid();
	if (nvmm_pid < 0) {
		if (verbose_mode)
			fprintf(stderr, "find_llama: nvmm process not found in /proc\n");
		return 0;
	}

	for (i = 0; i < sizeof(log_paths) / sizeof(log_paths[0]); i++) {
		parse_log_file(log_paths[i], regions, MAX_MEM_REGIONS,
			       &num_regions, agent_gpas, agent_gpa_present,
			       MAX_CPUS, &llama_gpa, &llama_gpa_present);
	}

	if (num_regions == 0 || !llama_gpa_present || llama_gpa == 0) {
		if (verbose_mode)
			fprintf(stderr, "find_llama: no NVMM_MEM or OVM_LLAMA GPA found\n");
		return 0;
	}

	uint64_t hva = 0;
	for (int r = 0; r < num_regions; r++) {
		if (llama_gpa >= regions[r].gpa &&
		    llama_gpa < regions[r].gpa + regions[r].size) {
			hva = regions[r].hva + (llama_gpa - regions[r].gpa);
			break;
		}
	}

	if (!hva) {
		if (verbose_mode)
			fprintf(stderr, "find_llama: GPA 0x%llx outside NVMM regions\n",
				(unsigned long long)llama_gpa);
		return 0;
	}

	if (!verify_llama_magic_at_hva(nvmm_pid, hva)) {
		if (verbose_mode)
			fprintf(stderr, "find_llama: HVA 0x%llx magic check failed\n",
				(unsigned long long)hva);
		return 0;
	}

	uint64_t hpa = translate_hva_to_hpa(nvmm_pid, hva);
	if (!hpa) {
		if (verbose_mode)
			fprintf(stderr, "find_llama: HVA 0x%llx pagemap lookup returned 0\n",
				(unsigned long long)hva);
		return 0;
	}

	if (mem_fd >= 0 && !verify_llama_magic_at_hpa(mem_fd, hpa)) {
		if (verbose_mode)
			fprintf(stderr, "find_llama: HPA 0x%llx magic check failed on mem_fd (continuing with verified HVA)\n",
				(unsigned long long)hpa);
	}

	if (discovered_offset)
		*discovered_offset = hpa;

	if (mem_fd >= 0) {
		uint64_t page_hpas[(sizeof(struct ovm_llama_stream) + 4095) / 4096];
		resolve_stream_page_hpas(mem_fd, hpa, page_hpas);
	}

	if (verbose_mode)
		fprintf(stderr, "find_llama: GPA 0x%llx -> HVA 0x%llx -> HPA 0x%llx (VERIFIED)\n",
			(unsigned long long)llama_gpa,
			(unsigned long long)hva,
			(unsigned long long)hpa);

	return 1;
}

#define OVM_LLAMA_STREAM_PAGES ((sizeof(struct ovm_llama_stream) + 4095) / 4096)
#define OVM_LLAMA_HPA_TABLE_MAGIC 0x4850415f54424c01ULL

static uint64_t find_llama_hva_for_gpa(pid_t nvmm_pid, uint64_t llama_gpa)
{
	const char *log_paths[] = {
		"/tmp/nvmm.log",
		"/tmp/ovm/nvmm.log",
		"/tmp/guest_monitor.log",
		"/var/log/nvmm.log"
	};
	struct nvmm_mem_region regions[MAX_MEM_REGIONS];
	uint64_t agent_gpas[MAX_CPUS];
	uint8_t agent_gpa_present[MAX_CPUS];
	uint64_t dummy_gpa = 0;
	uint8_t dummy_present = 0;
	int num_regions = 0;
	size_t i;

	memset(regions, 0, sizeof(regions));
	for (i = 0; i < sizeof(log_paths) / sizeof(log_paths[0]); i++) {
		parse_log_file(log_paths[i], regions, MAX_MEM_REGIONS,
			       &num_regions, agent_gpas, agent_gpa_present,
			       MAX_CPUS, &dummy_gpa, &dummy_present);
	}

	for (int r = 0; r < num_regions; r++) {
		if (llama_gpa >= regions[r].gpa &&
		    llama_gpa < regions[r].gpa + regions[r].size) {
			uint64_t cand_hva = regions[r].hva + (llama_gpa - regions[r].gpa);
			if (verify_llama_magic_at_hva(nvmm_pid, cand_hva))
				return cand_hva;
		}
	}

	/* Fallback: scan /proc/<nvmm_pid>/maps for shared memory mappings */
	char maps_path[64];
	snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)nvmm_pid);
	FILE *fp = fopen(maps_path, "r");
	if (fp) {
		char line[512];
		while (fgets(line, sizeof(line), fp)) {
			unsigned long long start_va = 0, end_va = 0, pgoff = 0;
			char perms[8] = {0};
			if (sscanf(line, "%llx-%llx %7s %llx", &start_va, &end_va, perms, &pgoff) == 4) {
				if (perms[0] == 'r' && perms[1] == 'w' && perms[3] == 's') {
					uint64_t map_size = end_va - start_va;
					if (map_size >= 64 * 1024 * 1024 && llama_gpa < map_size) {
						uint64_t cand_hva = start_va + llama_gpa;
						if (verify_llama_magic_at_hva(nvmm_pid, cand_hva)) {
							fclose(fp);
							return cand_hva;
						}
					}
				}
			}
		}
		fclose(fp);
	}

	return 0;
}

static void save_hpa_cache_file(uint64_t base_hpa, const uint64_t *page_hpas)
{
	int cfd = open("/tmp/ovm_llama_hpas.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (cfd >= 0) {
		uint64_t magic = OVM_LLAMA_HPA_TABLE_MAGIC;
		if (write(cfd, &magic, sizeof(magic)) == sizeof(magic) &&
		    write(cfd, &base_hpa, sizeof(base_hpa)) == sizeof(base_hpa)) {
			ssize_t ignored = write(cfd, page_hpas,
						OVM_LLAMA_STREAM_PAGES * sizeof(uint64_t));
			(void)ignored;
		}
		close(cfd);
	}

	FILE *tf = fopen("/tmp/llama_hpa.txt", "w");
	if (tf) {
		fprintf(tf, "0x%llx\n", (unsigned long long)base_hpa);
		fclose(tf);
	}

	/* Also preserve HPA table in its dedicated LUO session across kexec */
	int rc = system("/root/ovm/ovm_hpa_session --save >/dev/null 2>&1 || ./ovm_hpa_session --save >/dev/null 2>&1");
	(void)rc;
}

static int load_hpa_cache_file(uint64_t base_hpa, uint64_t *page_hpas)
{
	int cfd = open("/tmp/ovm_llama_hpas.bin", O_RDONLY);
	if (cfd >= 0) {
		uint64_t magic = 0, cached_base = 0;
		if (read(cfd, &magic, sizeof(magic)) == sizeof(magic) &&
		    magic == OVM_LLAMA_HPA_TABLE_MAGIC &&
		    read(cfd, &cached_base, sizeof(cached_base)) == sizeof(cached_base) &&
		    cached_base == base_hpa) {
			ssize_t sz = OVM_LLAMA_STREAM_PAGES * sizeof(uint64_t);
			if (read(cfd, page_hpas, sz) == sz && page_hpas[0] == base_hpa) {
				close(cfd);
				return 1;
			}
		}
		close(cfd);
	}
	return 0;
}

static void resolve_stream_page_hpas(int mem_fd, uint64_t base_hpa, uint64_t *page_hpas)
{
	struct ovm_llama_stream hdr;
	ssize_t hdr_sz = offsetof(struct ovm_llama_stream, buffer);
	pid_t nvmm_pid;

	for (size_t p = 0; p < OVM_LLAMA_STREAM_PAGES; p++)
		page_hpas[p] = base_hpa + p * 4096;

	if (pread(mem_fd, &hdr, hdr_sz, (off_t)base_hpa) < hdr_sz ||
	    hdr.magic != OVM_LLAMA_MAGIC)
		return;

	/* 1. Check if embedded HPA table in Page 256 is already populated in RAM */
	if (hdr.reserved[1] == OVM_LLAMA_HPA_TABLE_MAGIC && hdr.reserved[0] != 0) {
		uint64_t tbl_hpa = hdr.reserved[0];
		uint64_t tmp_hpas[OVM_LLAMA_STREAM_PAGES];
		ssize_t tbl_sz = sizeof(tmp_hpas);
		if (pread(mem_fd, tmp_hpas, tbl_sz, (off_t)tbl_hpa) == tbl_sz &&
		    tmp_hpas[0] == base_hpa && tmp_hpas[1] != 0) {
			memcpy(page_hpas, tmp_hpas, tbl_sz);
			return;
		}
	}

	/* 2. If nvmm is running, translate all 257 4KB pages via pagemap */
	nvmm_pid = find_nvmm_pid();
	if (nvmm_pid > 0 && hdr.gpa != 0) {
		uint64_t hva_0 = find_llama_hva_for_gpa(nvmm_pid, hdr.gpa);
		if (hva_0 != 0) {
			size_t valid_count = 0;
			for (size_t p = 0; p < OVM_LLAMA_STREAM_PAGES; p++) {
				uint64_t hpa = translate_hva_to_hpa(nvmm_pid, hva_0 + p * 4096);
				if (hpa != 0) {
					page_hpas[p] = hpa;
					valid_count++;
				}
			}

			if (valid_count == OVM_LLAMA_STREAM_PAGES) {
				char proc_mem[64];
				snprintf(proc_mem, sizeof(proc_mem), "/proc/%d/mem", (int)nvmm_pid);
				int pm_fd = open(proc_mem, O_RDWR);
				if (pm_fd >= 0) {
					uint64_t tbl_hva = hva_0 + 256 * 4096 + 256;
					uint64_t tbl_hpa = page_hpas[256] + 256;
					ssize_t tbl_sz = OVM_LLAMA_STREAM_PAGES * sizeof(uint64_t);
					if (pwrite(pm_fd, page_hpas, tbl_sz, (off_t)tbl_hva) == tbl_sz) {
						uint64_t res_meta[2] = { tbl_hpa, OVM_LLAMA_HPA_TABLE_MAGIC };
						off_t res_off = (off_t)(hva_0 + offsetof(struct ovm_llama_stream, reserved));
						ssize_t ignored = pwrite(pm_fd, res_meta, sizeof(res_meta), res_off);
						(void)ignored;
					}
					close(pm_fd);
				}
				save_hpa_cache_file(base_hpa, page_hpas);
				return;
			}
		}
	}

	/* 3. Fallback to file cache across kexec */
	load_hpa_cache_file(base_hpa, page_hpas);
}

static int read_llama_stream(int fd, uint64_t offset, uint64_t start_pos,
			     int raw_mode, int wait_ms, int header_only)
{
	uint64_t cur_pos = start_pos;
	uint64_t page_hpas[OVM_LLAMA_STREAM_PAGES];
	char chunk[4096];
	int hpas_resolved = 0;

	resolve_stream_page_hpas(fd, offset, page_hpas);
	if (page_hpas[1] != offset + 4096)
		hpas_resolved = 1;

	do {
		struct ovm_llama_stream hdr;
		ssize_t hdr_sz = offsetof(struct ovm_llama_stream, buffer);

		if (pread(fd, &hdr, hdr_sz, (off_t)offset) < hdr_sz)
			return -1;

		if (hdr.magic != OVM_LLAMA_MAGIC)
			return -1;

		if (!hpas_resolved && hdr.total_chars > 3000) {
			resolve_stream_page_hpas(fd, offset, page_hpas);
			hpas_resolved = 1;
		}

		if (!raw_mode && wait_ms == 0) {
			printf("STATUS %u TOKENS %llu TOTAL_CHARS %llu HEAD %llu\n",
			       hdr.status,
			       (unsigned long long)hdr.total_tokens,
			       (unsigned long long)hdr.total_chars,
			       (unsigned long long)hdr.head);
		}

		if (header_only)
			return 0;

		if (hdr.total_chars > cur_pos) {
			if (hdr.total_chars - cur_pos > OVM_LLAMA_BUF_SIZE)
				cur_pos = hdr.total_chars - OVM_LLAMA_BUF_SIZE;

			uint64_t remaining = hdr.total_chars - cur_pos;
			while (remaining > 0) {
				uint64_t buf_idx = cur_pos % OVM_LLAMA_BUF_SIZE;
				uint64_t struct_off = offsetof(struct ovm_llama_stream, buffer) + buf_idx;
				uint64_t page_idx = struct_off / 4096;
				uint64_t page_off = struct_off % 4096;

				size_t chunk_len = remaining > sizeof(chunk) ? sizeof(chunk) : remaining;
				if (page_off + chunk_len > 4096)
					chunk_len = 4096 - page_off;
				if (buf_idx + chunk_len > OVM_LLAMA_BUF_SIZE)
					chunk_len = OVM_LLAMA_BUF_SIZE - buf_idx;

				if (page_idx >= OVM_LLAMA_STREAM_PAGES || page_hpas[page_idx] == 0)
					break;

				uint64_t read_hpa = page_hpas[page_idx] + page_off;
				if (pread(fd, chunk, chunk_len, (off_t)read_hpa) != (ssize_t)chunk_len)
					break;

				fwrite(chunk, 1, chunk_len, stdout);
				cur_pos += chunk_len;
				remaining -= chunk_len;
			}
			fflush(stdout);
		}

		if (wait_ms <= 0)
			break;

		if (hdr.status == OVM_LLAMA_STATUS_FINISHED ||
		    hdr.status == OVM_LLAMA_STATUS_ERROR)
			break;

		usleep(wait_ms * 1000);
	} while (1);

	return 0;
}

static int read_loop_at_offset(int fd, uint64_t offset, int target_cpu,
			       uint64_t *loop_out)
{
	uint64_t header[3];
	uint64_t pgsz = 4096;
	uint64_t page_off = offset & ~(pgsz - 1);
	uint64_t in_page = offset & (pgsz - 1);

	if (is_regular_file(fd)) {
		void *map = mmap(NULL, pgsz, PROT_READ, MAP_SHARED, fd,
				 (off_t)page_off);
		if (map != MAP_FAILED) {
			const volatile uint64_t *words =
				(const volatile uint64_t *)((const char *)map + in_page);
			if (words[0] == OVM_MAGIC0 &&
			    OVM_MAGIC1_MATCH(words[1])) {
				int found_cpu = OVM_MAGIC1_GET_CPU(words[1]);
				if (target_cpu < 0 ||
				    target_cpu == found_cpu) {
					*loop_out = words[OVM_DATA_START_IDX];
					munmap(map, pgsz);
					return 0;
				}
			}
			munmap(map, pgsz);
		}
	}

	if (pread(fd, header, sizeof(header), (off_t)offset) ==
	    (ssize_t)sizeof(header)) {
		if (header[0] == OVM_MAGIC0 &&
		    OVM_MAGIC1_MATCH(header[1])) {
			int found_cpu = OVM_MAGIC1_GET_CPU(header[1]);
			if (target_cpu < 0 || target_cpu == found_cpu) {
				*loop_out = header[OVM_DATA_START_IDX];
				return 0;
			}
		}
	}

	return -1;
}

static int read_multiple_offsets(int fd, const char *offsets_str,
				 int target_cpu)
{
	char *str = strdup(offsets_str);
	char *token;
	char *rest;
	int first = 1;

	if (!str)
		return -1;

	rest = str;
	while ((token = strtok_r(rest, ",", &rest))) {
		uint64_t off = strtoull(token, NULL, 0);
		uint64_t loop_val = 0;

		if (read_loop_at_offset(fd, off, target_cpu, &loop_val) < 0) {
			free(str);
			return -1;
		}
		if (!first)
			printf(" ");
		printf("0x%llx", (unsigned long long)loop_val);
		first = 0;
	}
	printf("\n");
	free(str);
	return 0;
}

int main(int argc, char **argv)
{
	const char *file_path = NULL;
	const char *target_offsets_str = NULL;
	int find_mode = 0;
	int all_cpus_mode = 0;
	int offset_specified = 0;
	int find_llama_mode = 0;
	int stream_mode = 0;
	uint64_t stream_offset = 0;
	uint64_t stream_pos = 0;
	int raw_mode = 0;
	int wait_ms = 0;
	int header_only = 0;
	int target_cpu = -1;
	int fd;
	int opt;

	while ((opt = getopt(argc, argv, "FaO:c:f:vhLS:p:rw:H")) != -1) {
		switch (opt) {
		case 'F':
			find_mode = 1;
			break;
		case 'a':
			all_cpus_mode = 1;
			break;
		case 'O':
			offset_specified = 1;
			target_offsets_str = optarg;
			break;
		case 'L':
			find_llama_mode = 1;
			break;
		case 'S':
			stream_mode = 1;
			stream_offset = strtoull(optarg, NULL, 0);
			break;
		case 'H':
			header_only = 1;
			break;
		case 'p':
			stream_pos = strtoull(optarg, NULL, 0);
			break;
		case 'r':
			raw_mode = 1;
			break;
		case 'w':
			wait_ms = atoi(optarg);
			break;
		case 'c':
			target_cpu = atoi(optarg);
			break;
		case 'f':
			file_path = optarg;
			break;
		case 'v':
			verbose_mode = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	int mode_count = find_mode + offset_specified + find_llama_mode + stream_mode;
	if (mode_count != 1)
		usage(argv[0]);

	if (!file_path) {
		if (access("/dev/mem", R_OK) == 0) {
			file_path = "/dev/mem";
		} else if (find_mode || find_llama_mode) {
			file_path = "/dev/null";
		} else {
			errx(1, "Error: No memory file. Use -f <path>");
		}
	}

	fd = open(file_path, O_RDONLY);
	if (fd < 0)
		err(1, "Error opening %s", file_path);

	if (find_llama_mode) {
		uint64_t t0 = get_time_ns();
		uint64_t discovered_offset = 0;

		if (find_llama_via_pagemap(fd, &discovered_offset)) {
			uint64_t elapsed_ns = get_time_ns() - t0;
			if (verbose_mode)
				fprintf(stderr,
					"find_llama: resolved via pagemap to 0x%llx in %.2f ms\n",
					(unsigned long long)discovered_offset,
					(double)elapsed_ns / 1000000.0);

			printf("0x%llx\n",
			       (unsigned long long)discovered_offset);
			close(fd);
			return 0;
		}
		close(fd);
		return 2;
	}

	if (stream_mode) {
		if (read_llama_stream(fd, stream_offset, stream_pos, raw_mode, wait_ms, header_only) < 0) {
			close(fd);
			errx(1, "Error reading LLAMA stream from offset 0x%llx",
			     (unsigned long long)stream_offset);
		}
		close(fd);
		return 0;
	}

	if (find_mode) {
		uint64_t t0 = get_time_ns();

		if (all_cpus_mode) {
			uint64_t offsets[MAX_CPUS];
			int found_count = 0;
			int i;

			for (i = 0; i < MAX_CPUS; i++)
				offsets[i] = 0;

			find_agents_via_pagemap(fd, -1, offsets, MAX_CPUS,
						&found_count, NULL);

			if (found_count > 0) {
				uint64_t elapsed_ns = get_time_ns() - t0;
				if (verbose_mode)
					fprintf(stderr,
						"find_all: resolved %d CPUs via pagemap in %.2f ms\n",
						found_count, (double)elapsed_ns / 1000000.0);

				for (i = 0; i < MAX_CPUS; i++) {
					if (offsets[i] != 0)
						printf("%d 0x%llx\n", i,
						       (unsigned long long)offsets[i]);
				}
				close(fd);
				return 0;
			}
			close(fd);
			return 2;
		}

		uint64_t discovered_offset = 0;
		int found_count = 0;

		find_agents_via_pagemap(fd, target_cpu, NULL, MAX_CPUS,
					&found_count, &discovered_offset);

		if (discovered_offset != 0) {
			uint64_t elapsed_ns = get_time_ns() - t0;
			if (verbose_mode)
				fprintf(stderr,
					"find_cpu %d: resolved via pagemap to 0x%llx in %.2f ms\n",
					target_cpu, (unsigned long long)discovered_offset,
					(double)elapsed_ns / 1000000.0);

			printf("0x%llx\n",
			       (unsigned long long)discovered_offset);
			close(fd);
			return 0;
		}
		close(fd);
		return 2;
	}

	if (offset_specified) {
		if (read_multiple_offsets(fd, target_offsets_str,
					  target_cpu) < 0) {
			close(fd);
			errx(1, "Error reading loop from offset(s) %s",
			     target_offsets_str);
		}
		close(fd);
		return 0;
	}

	close(fd);
	return 0;
}
