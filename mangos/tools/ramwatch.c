/*
 * ramwatch: watches the RAM and reports at which physical address it gets overwritten.
 *
 * It allocates and locks the requested megabytes, writes into every word a value that depends on
 * its position, then keeps reading everything back. If a page no longer holds what was written,
 * someone else wrote over it: the program prints the physical address of the page (from
 * /proc/self/pagemap, needs root), how many words changed and what the first one holds now, then
 * rewrites it and goes on. It finds memory the kernel believes is free but another piece of
 * hardware is using.
 *
 * Usage: ramwatch <megabytes> <seconds>
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096UL

static uint64_t expected(uint64_t *word)
{
	return (uint64_t)(uintptr_t)word ^ 0x5a5aa5a5c3c33c3cULL;
}

static uint64_t phys_of(int pagemap, void *addr)
{
	uint64_t entry;
	off_t off = (uintptr_t)addr / PAGE * sizeof(entry);

	if (pread(pagemap, &entry, sizeof(entry), off) != sizeof(entry) || !(entry & (1ULL << 63)))
		return 0;
	return (entry & ((1ULL << 55) - 1)) * PAGE;
}

int main(int argc, char **argv)
{
	size_t bytes, words, i;
	uint64_t *mem, lowest = ~0ULL, highest = 0;
	long seconds, dirty_pages = 0, passes = 0;
	time_t start;
	int pagemap;

	if (argc != 3) {
		fprintf(stderr, "usage: ramwatch <megabytes> <seconds>\n");
		return 2;
	}
	bytes = strtoul(argv[1], NULL, 10) << 20;
	seconds = strtol(argv[2], NULL, 10);
	words = bytes / sizeof(*mem);

	mem = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		perror("ramwatch: memory not allocated");
		return 1;
	}
	/* 4 kB pages, so every error is located precisely. */
	madvise(mem, bytes, MADV_NOHUGEPAGE);
	for (i = 0; i < words; i++)
		mem[i] = expected(&mem[i]);
	if (mlock(mem, bytes)) {
		perror("ramwatch: memory not locked");
		return 1;
	}
	pagemap = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap < 0) {
		perror("ramwatch: /proc/self/pagemap");
		return 1;
	}
	for (i = 0; i < words; i += PAGE / sizeof(*mem)) {
		uint64_t phys = phys_of(pagemap, &mem[i]);

		if (phys && phys < lowest)
			lowest = phys;
		if (phys > highest)
			highest = phys;
	}
	if (!highest) {
		fprintf(stderr, "ramwatch: physical addresses not readable from /proc/self/pagemap: root needed\n");
		return 1;
	}
	printf("watching %zu MB, physical addresses from 0x%llx to 0x%llx\n", bytes >> 20,
	       (unsigned long long)lowest, (unsigned long long)highest);
	fflush(stdout);

	start = time(NULL);
	while (time(NULL) - start < seconds) {
		for (i = 0; i < words; i += PAGE / sizeof(*mem)) {
			size_t w, wrong = 0, first = 0;

			for (w = 0; w < PAGE / sizeof(*mem); w++) {
				if (mem[i + w] != expected(&mem[i + w])) {
					if (!wrong)
						first = w;
					wrong++;
				}
			}
			if (!wrong)
				continue;
			dirty_pages++;
			printf("t=%lds physical page 0x%llx: %zu words out of %lu changed, the first now holds 0x%016llx\n",
			       (long)(time(NULL) - start), (unsigned long long)phys_of(pagemap, &mem[i]),
			       wrong, PAGE / sizeof(*mem), (unsigned long long)mem[i + first]);
			fflush(stdout);
			for (w = 0; w < PAGE / sizeof(*mem); w++)
				mem[i + w] = expected(&mem[i + w]);
		}
		passes++;
	}
	printf("done: %ld passes, %ld pages dirtied\n", passes, dirty_pages);
	return 0;
}
