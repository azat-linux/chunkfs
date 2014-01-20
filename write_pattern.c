/*
 * Write a pattern to a file.
 *
 * (C) 2007 Valerie Henson <val@nmt.edu>
 */

#include <stdio.h>
#include <stdlib.h>
#include <error.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#define FILE_SIZE (32 * 1024 * 1024)

static char * cmd;

static void usage (void)
{
	fprintf(stderr, "Usage: %s <file>\n", cmd);
	exit(1);
}

int main (int argc, char * argv[])
{
	int fd;
	char * file;
	char buf[4096];
	int n = 0;
	int written = 0;

	cmd = argv[0];

	if (argc != 2)
		usage();

	file = argv[1];

	if ((fd = open(file, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR)) < 0) {
		error(0, errno, "Cannot open file %s", file);
		usage();
	}

	memset(buf, '5', sizeof(buf));

	while (written < FILE_SIZE) {
		n = write(fd, buf, sizeof(buf));
		if (n == -1) {
			error(0, errno, "Cannot write file %s", file);
			usage();
		}
		written += n;
	}

	close(fd);

	return 0;
}
