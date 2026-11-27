/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/* sgxsnap MS [OFF...]: sample the SGX register window (read only) for MS ms. The window is
 * at SGXSNAP_BASE (default 0x50000000, the OMAP3's). Read only while the GPU is powered and
 * clocked (runtime PM control=on, runtime_status active): an unclocked read can hang a SoC.
 * Without OFFs: every word 0x000-0xffc except the 2D FIFO pair (0xe10, 0xe14) and 0xca8. With OFFs:
 * only those (faster). Prints the first sample's nonzero words ("F off val"), then each
 * change as "C t_us off old new", and "N samples elapsed_us" at the end. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static long long now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
	static unsigned offs[1024], prev[1024], cur[1024];
	unsigned n = 0, i;
	long long t0, t, end, samples = 0;
	int fd;
	volatile unsigned *r;

	if (argc < 2) {
		fprintf(stderr, "usage: sgxsnap MS [OFF...]\n");
		return 2;
	}
	if (argc > 2) {
		for (i = 2; i < (unsigned)argc && n < 1024; i++) {
			unsigned o = strtoul(argv[i], 0, 0) & 0xffc;

			if (o != 0xe10 && o != 0xe14)
				offs[n++] = o;
		}
	} else {
		for (i = 0; i < 0x1000; i += 4)	/* not BIF_MEM_REQ_STAT: it toggles all the time */
			if (i != 0xe10 && i != 0xe14 && i != 0xca8)
				offs[n++] = i;
	}
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	r = mmap(0, 0x1000, PROT_READ, MAP_SHARED, fd,
		 getenv("SGXSNAP_BASE") ? strtoul(getenv("SGXSNAP_BASE"), 0, 0) : 0x50000000);
	if (r == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	t0 = now_us();
	end = t0 + atoll(argv[1]) * 1000;
	for (i = 0; i < n; i++)
		prev[i] = r[offs[i] / 4];
	for (i = 0; i < n; i++)
		if (prev[i])
			printf("F 0x%03x 0x%08x\n", offs[i], prev[i]);
	do {
		for (i = 0; i < n; i++)
			cur[i] = r[offs[i] / 4];
		t = now_us();
		samples++;
		for (i = 0; i < n; i++)
			if (cur[i] != prev[i]) {
				printf("C %lld 0x%03x 0x%08x 0x%08x\n", t - t0, offs[i], prev[i], cur[i]);
				prev[i] = cur[i];
			}
	} while (t < end);
	printf("N %lld %lld\n", samples, now_us() - t0);
	return 0;
}
