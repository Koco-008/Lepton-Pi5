#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "LEPTON_OEM.h"
#include "LEPTON_SDK.h"
#include "LEPTON_Types.h"

#define DEFAULT_BOOT_TIMEOUT_MS 6000U
#define BOOT_POLL_INTERVAL_MS 100U

struct options {
	bool reboot_only;
	unsigned int boot_timeout_ms;
};

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void sleep_milliseconds(unsigned int milliseconds)
{
	struct timespec delay = {
		.tv_sec = (time_t)(milliseconds / 1000U),
		.tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
	};

	while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
		;
}

static int parse_unsigned(const char *text, unsigned int *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX)
		return -1;
	*value = (unsigned int)parsed;
	return 0;
}

static void usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s [--configure | --reboot-only] [--boot-timeout-ms MS]\n",
		program);
}

static int parse_options(int argc, char **argv, struct options *options)
{
	enum {
		OPTION_CONFIGURE = 1000,
		OPTION_REBOOT_ONLY,
		OPTION_BOOT_TIMEOUT,
	};
	static const struct option long_options[] = {
		{ "configure", no_argument, NULL, OPTION_CONFIGURE },
		{ "reboot-only", no_argument, NULL, OPTION_REBOOT_ONLY },
		{ "boot-timeout-ms", required_argument, NULL, OPTION_BOOT_TIMEOUT },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int option;

	*options = (struct options) {
		.boot_timeout_ms = DEFAULT_BOOT_TIMEOUT_MS,
	};
	while ((option = getopt_long(argc, argv, "h", long_options, NULL)) != -1) {
		switch (option) {
		case OPTION_CONFIGURE:
			options->reboot_only = false;
			break;
		case OPTION_REBOOT_ONLY:
			options->reboot_only = true;
			break;
		case OPTION_BOOT_TIMEOUT:
			if (parse_unsigned(optarg, &options->boot_timeout_ms) != 0 ||
			    options->boot_timeout_ms == 0)
				return -1;
			break;
		case 'h':
			usage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
		default:
			return -1;
		}
	}
	return optind == argc ? 0 : -1;
}

static LEP_RESULT open_camera(LEP_CAMERA_PORT_DESC_T *port)
{
	memset(port, 0, sizeof(*port));
	return LEP_OpenPort(1, LEP_CCI_TWI, 400, port);
}

static int wait_for_boot(LEP_CAMERA_PORT_DESC_T *port, unsigned int timeout_ms)
{
	uint64_t deadline = monotonic_milliseconds() + timeout_ms;
	LEP_RESULT last_result = LEP_COMM_NO_DEV;

	for (;;) {
		LEP_SDK_BOOT_STATUS_E boot_status = LEP_BOOT_STATUS_NOT_BOOTED;

		last_result = open_camera(port);
		if (last_result == LEP_OK) {
			last_result = LEP_GetCameraBootStatus(port, &boot_status);
			if (last_result == LEP_OK &&
			    boot_status == LEP_BOOT_STATUS_BOOTED)
				return 0;
			(void)LEP_ClosePort(port);
		}
		if (monotonic_milliseconds() >= deadline)
			break;
		sleep_milliseconds(BOOT_POLL_INTERVAL_MS);
	}

	fprintf(stderr, "Lepton did not become ready within %u ms (result=%d)\n",
		timeout_ms, last_result);
	return -1;
}

static int configure_vsync(const struct options *options)
{
	LEP_CAMERA_PORT_DESC_T port;
	LEP_OEM_GPIO_MODE_E mode = LEP_OEM_END_GPIO_MODE;
	LEP_RESULT result;
	int return_code = -1;

	if (wait_for_boot(&port, options->boot_timeout_ms) != 0)
		return -1;

	result = LEP_GetOemGpioMode(&port, &mode);
	if (result != LEP_OK) {
		fprintf(stderr, "LEP_GetOemGpioMode failed: %d\n", result);
		goto done;
	}
	fprintf(stderr, "Lepton GPIO mode before configure: %d\n", mode);

	result = LEP_SetOemGpioMode(&port, LEP_OEM_GPIO_MODE_VSYNC);
	if (result != LEP_OK) {
		fprintf(stderr, "LEP_SetOemGpioMode(VSYNC) failed: %d\n", result);
		goto done;
	}

	mode = LEP_OEM_END_GPIO_MODE;
	result = LEP_GetOemGpioMode(&port, &mode);
	if (result != LEP_OK || mode != LEP_OEM_GPIO_MODE_VSYNC) {
		fprintf(stderr, "Lepton VSYNC verification failed: result=%d mode=%d\n",
			result, mode);
		goto done;
	}

	fprintf(stderr, "Lepton GPIO mode configured and verified: %d\n", mode);
	return_code = 0;

done:
	(void)LEP_ClosePort(&port);
	return return_code;
}

static int reboot_camera(void)
{
	LEP_CAMERA_PORT_DESC_T port;
	LEP_RESULT result = open_camera(&port);

	if (result != LEP_OK) {
		fprintf(stderr, "Unable to open Lepton control port: %d\n", result);
		return -1;
	}

	fprintf(stderr, "Issuing LEP_RunOemReboot\n");
	result = LEP_RunOemReboot(&port);
	(void)LEP_ClosePort(&port);
	if (result != LEP_OK) {
		fprintf(stderr,
			"LEP_RunOemReboot returned %d; the camera may already be rebooting\n",
			result);
		return -1;
	}
	fprintf(stderr, "LEP_RunOemReboot accepted\n");
	return 0;
}

int main(int argc, char **argv)
{
	struct options options;

	if (parse_options(argc, argv, &options) != 0) {
		usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

	return (options.reboot_only ? reboot_camera() : configure_vsync(&options)) == 0 ?
		EXIT_SUCCESS : EXIT_FAILURE;
}
