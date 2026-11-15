// SPDX-License-Identifier: GPL-2.0
/*
 * i2c-rd BUS ADDR FIRST COUNT: read COUNT 8-bit registers starting at FIRST, one combined
 * write-address/read-byte transfer each, and print "reg:value" lines. Reads only: for
 * comparing register dumps on systems without i2c-tools. Works with a bound driver.
 */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
	char path[32];
	int fd, i, first, count, addr;

	if (argc != 5) {
		fprintf(stderr, "usage: i2c-rd BUS ADDR FIRST COUNT\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/dev/i2c-%d", atoi(argv[1]));
	addr = strtol(argv[2], NULL, 0);
	first = strtol(argv[3], NULL, 0);
	count = strtol(argv[4], NULL, 0);

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	for (i = first; i < first + count; i++) {
		unsigned char reg = i, val = 0;
		struct i2c_msg msgs[2] = {
			{ .addr = addr, .flags = 0, .len = 1, .buf = &reg },
			{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = &val },
		};
		struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };

		if (ioctl(fd, I2C_RDWR, &data) < 0)
			printf("%02x:--\n", i);
		else
			printf("%02x:%02x\n", i, val);
	}

	return 0;
}
