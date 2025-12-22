/*
 * LiME ioctl Control Tool
 *
 * Userspace tool for controlling LiME when compiled with CONFIG_LIME_STEALTH
 * Communicates via ioctl on /dev/lwis-sensor-imx461
 *
 * Compile for Android:
 *   aarch64-linux-android-gcc -static -o lime_ioctl lime_ioctl.c
 *
 * Usage:
 *   ./lime_ioctl -1 /data/local/tmp/full.lime           # Full memory
 *   ./lime_ioctl 1234 /data/local/tmp/process.lime      # Process 1234
 *   ./lime_ioctl -p 1234 -o /data/dump.lime -f lime -d sha256
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <getopt.h>

#define LIME_IOCTL_MAGIC 0x4C4D4531  /* "LME1" */
#define LIME_DEVICE "/dev/lwis-sensor-imx461"

struct lime_ioctl_config {
	int target_pid;           /* -1 = full memory, >0 = process PID */
	char path[256];           /* Output path or tcp:port */
	char format[16];          /* raw, lime, or padded */
	char digest[32];          /* Hash algorithm (optional) */
	int dio;                  /* Direct I/O flag */
	int localhostonly;        /* TCP localhost only */
};

static void usage(const char *prog)
{
	fprintf(stderr, "LiME ioctl Control Tool\n\n");
	fprintf(stderr, "Usage: %s [OPTIONS] <pid> <output_path>\n\n", prog);
	fprintf(stderr, "Arguments:\n");
	fprintf(stderr, "  pid            Process ID to dump (-1 for full memory)\n");
	fprintf(stderr, "  output_path    Destination file or tcp:port\n\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -f, --format FORMAT    Output format: raw, lime, padded (default: lime)\n");
	fprintf(stderr, "  -d, --digest ALGO      Hash algorithm: md5, sha1, sha256, etc.\n");
	fprintf(stderr, "  -D, --dio              Enable Direct I/O\n");
	fprintf(stderr, "  -l, --localhost-only   TCP localhost only\n");
	fprintf(stderr, "  -h, --help             Show this help\n\n");
	fprintf(stderr, "Examples:\n");
	fprintf(stderr, "  %s -1 /data/full.lime                    # Full memory dump\n", prog);
	fprintf(stderr, "  %s 1234 /data/process.lime               # Dump process 1234\n", prog);
	fprintf(stderr, "  %s -f raw -d sha256 -1 /data/dump.raw    # Full dump, raw format, SHA256\n", prog);
	fprintf(stderr, "  %s 5678 tcp:4444                         # Dump process 5678 over TCP\n", prog);
}

int main(int argc, char **argv)
{
	struct lime_ioctl_config cfg = {
		.target_pid = -1,
		.dio = 0,
		.localhostonly = 0,
	};
	int fd, ret, opt;
	int pid_arg = -1, path_arg_idx = -1;

	strcpy(cfg.format, "lime");  /* Default format */
	cfg.digest[0] = '\0';        /* No digest by default */

	static struct option long_options[] = {
		{"format",         required_argument, 0, 'f'},
		{"digest",         required_argument, 0, 'd'},
		{"dio",            no_argument,       0, 'D'},
		{"localhost-only", no_argument,       0, 'l'},
		{"help",           no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	/* Parse options */
	while ((opt = getopt_long(argc, argv, "f:d:Dlh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'f':
			strncpy(cfg.format, optarg, sizeof(cfg.format) - 1);
			cfg.format[sizeof(cfg.format) - 1] = '\0';
			break;
		case 'd':
			strncpy(cfg.digest, optarg, sizeof(cfg.digest) - 1);
			cfg.digest[sizeof(cfg.digest) - 1] = '\0';
			break;
		case 'D':
			cfg.dio = 1;
			break;
		case 'l':
			cfg.localhostonly = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	/* Parse positional arguments: <pid> <path> */
	if (optind + 2 != argc) {
		fprintf(stderr, "Error: Missing required arguments\n\n");
		usage(argv[0]);
		return 1;
	}

	cfg.target_pid = atoi(argv[optind]);
	strncpy(cfg.path, argv[optind + 1], sizeof(cfg.path) - 1);
	cfg.path[sizeof(cfg.path) - 1] = '\0';

	/* Validate */
	if (cfg.target_pid < -1) {
		fprintf(stderr, "Error: Invalid PID %d (must be >= -1)\n", cfg.target_pid);
		return 1;
	}

	if (strcmp(cfg.format, "raw") != 0 &&
	    strcmp(cfg.format, "lime") != 0 &&
	    strcmp(cfg.format, "padded") != 0) {
		fprintf(stderr, "Error: Invalid format '%s' (must be raw, lime, or padded)\n",
			cfg.format);
		return 1;
	}

	/* Open device */
	fd = open(LIME_DEVICE, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Error: Failed to open %s: %s\n",
			LIME_DEVICE, strerror(errno));
		fprintf(stderr, "Make sure LiME is compiled with CONFIG_LIME_STEALTH=y\n");
		return 1;
	}

	/* Display configuration */
	printf("LiME Acquisition Configuration:\n");
	printf("  Target: %s\n", cfg.target_pid == -1 ? "Full memory" : "Process");
	if (cfg.target_pid > 0)
		printf("  PID: %d\n", cfg.target_pid);
	printf("  Output: %s\n", cfg.path);
	printf("  Format: %s\n", cfg.format);
	if (cfg.digest[0])
		printf("  Digest: %s\n", cfg.digest);
	printf("  DIO: %s\n", cfg.dio ? "enabled" : "disabled");
	if (strncmp(cfg.path, "tcp:", 4) == 0)
		printf("  Localhost only: %s\n", cfg.localhostonly ? "yes" : "no");
	printf("\nTriggering acquisition...\n");

	/* Send ioctl */
	ret = ioctl(fd, LIME_IOCTL_MAGIC, &cfg);
	if (ret < 0) {
		fprintf(stderr, "Error: ioctl failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("Acquisition completed successfully!\n");
	close(fd);
	return 0;
}
