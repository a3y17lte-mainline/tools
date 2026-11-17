// SPDX-License-Identifier: GPL-2.0-or-later
// Build: clang --target=aarch64-alpine-linux-musl --sysroot=out/modem-sysroot -fuse-ld=lld -static -O2 -o ctl ctl.c
// Minimal ALSA control tool: ctl NAME [VALUE]  (VALUE: number, on/off, or enum item name)
#include <fcntl.h>
#include <sound/asound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: ctl NAME [VALUE]\n"); return 2; }
	int fd = open("/dev/snd/controlC0", O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	struct snd_ctl_elem_info info = {0};
	info.id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	snprintf((char *)info.id.name, sizeof info.id.name, "%s", argv[1]);
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { perror(argv[1]); return 1; }
	struct snd_ctl_elem_value v = {0};
	v.id = info.id;
	if (argc < 3) {
		if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) < 0) { perror("read"); return 1; }
		long x = info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED ? v.value.enumerated.item[0]
								     : v.value.integer.value[0];
		if (info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN) printf("%s\n", x ? "on" : "off");
		else if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
			info.value.enumerated.item = x;
			ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info);
			printf("%s\n", info.value.enumerated.name);
		} else printf("%ld\n", x);
		return 0;
	}
	const char *s = argv[2];
	long x;
	if (info.type == SNDRV_CTL_ELEM_TYPE_ENUMERATED) {
		unsigned n = info.value.enumerated.items, i;
		for (i = 0; i < n; i++) {
			info.value.enumerated.item = i;
			if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0) { perror("enum"); return 1; }
			if (!strcmp(info.value.enumerated.name, s)) break;
		}
		if (i == n) { fprintf(stderr, "%s: no item %s\n", argv[1], s); return 1; }
		for (unsigned c = 0; c < info.count; c++) v.value.enumerated.item[c] = i;
	} else {
		if (!strcmp(s, "on")) x = 1;
		else if (!strcmp(s, "off")) x = 0;
		else x = strtol(s, NULL, 0);
		for (unsigned c = 0; c < info.count; c++) v.value.integer.value[c] = x;
	}
	if (ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &v) < 0) { perror("write"); return 1; }
	return 0;
}
