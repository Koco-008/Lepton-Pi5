#define _POSIX_C_SOURCE 200809L

#include "frame_pipeline.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_INPUT_DEVICE "/dev/video0"
#define DEFAULT_RAW_DEVICE "/dev/video10"
#define DEFAULT_COLOR_DEVICE "/dev/video11"
#define DEFAULT_COLOR_WIDTH 640U
#define DEFAULT_COLOR_HEIGHT 480U
#define OUTPUT_FRAMES_PER_SECOND 9U
#define CAPTURE_BUFFER_COUNT 4U
#define OUTPUT_BUFFER_COUNT 4U
#define INPUT_POLL_TIMEOUT_MS 2000
#define OUTPUT_POLL_TIMEOUT_MS 1000
#define MAX_OUTPUT_DIMENSION 8192U
#define STALL_RECOVERY_EXIT_STATUS 75

enum streamer_result {
	STREAMER_ERROR = -1,
	STREAMER_OK = 0,
	STREAMER_STALLED = 1,
};

struct mapped_buffer {
	void *address;
	size_t length;
};

struct capture_device {
	int fd;
	const char *path;
	struct mapped_buffer *buffers;
	unsigned int buffer_count;
	bool streaming;
};

struct output_device {
	int fd;
	const char *path;
	struct mapped_buffer *buffers;
	unsigned int buffer_count;
	size_t frame_size;
	bool streaming;
};

struct options {
	const char *input_path;
	const char *raw_path;
	const char *color_path;
	unsigned int color_width;
	unsigned int color_height;
	unsigned int stall_timeout_seconds;
	uint64_t frame_limit;
	bool quiet;
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static int xioctl(int fd, unsigned long request, void *argument)
{
	int result;

	do {
		result = ioctl(fd, request, argument);
	} while (result < 0 && errno == EINTR);

	return result;
}

static uint32_t effective_caps(const struct v4l2_capability *capability)
{
	if ((capability->capabilities & V4L2_CAP_DEVICE_CAPS) != 0)
		return capability->device_caps;
	return capability->capabilities;
}

static void fourcc_to_string(uint32_t fourcc, char output[5])
{
	output[0] = (char)(fourcc & 0xffU);
	output[1] = (char)((fourcc >> 8) & 0xffU);
	output[2] = (char)((fourcc >> 16) & 0xffU);
	output[3] = (char)((fourcc >> 24) & 0x7fU);
	output[4] = '\0';
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

static int parse_u64(const char *text, uint64_t *value)
{
	char *end = NULL;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0')
		return -1;
	*value = (uint64_t)parsed;
	return 0;
}

static void print_usage(FILE *stream, const char *program)
{
	fprintf(stream,
		"Usage: %s [options]\n"
		"\n"
		"Assemble Lepton 3 VoSPI segments and publish two V4L2 streams.\n"
		"\n"
		"Options:\n"
		"  -i, --input PATH          VoSPI V4L2 input [%s]\n"
		"  -r, --raw-output PATH     160x120 Y16 output [%s]\n"
		"  -c, --color-output PATH   False-color YUYV output [%s]\n"
		"      --color-width PIXELS  False-color width [%u]\n"
		"      --color-height PIXELS False-color height [%u]\n"
		"      --stall-timeout SEC   Exit 75 after SEC without a complete frame [off]\n"
		"  -n, --frames COUNT        Stop after COUNT completed frames\n"
		"  -q, --quiet               Suppress periodic status output\n"
		"  -h, --help                Show this help\n",
		program,
		DEFAULT_INPUT_DEVICE,
		DEFAULT_RAW_DEVICE,
		DEFAULT_COLOR_DEVICE,
		DEFAULT_COLOR_WIDTH,
		DEFAULT_COLOR_HEIGHT);
}

static int parse_options(int argc, char **argv, struct options *options)
{
	enum {
		OPTION_COLOR_WIDTH = 1000,
		OPTION_COLOR_HEIGHT,
		OPTION_STALL_TIMEOUT,
	};
	static const struct option long_options[] = {
		{ "input", required_argument, NULL, 'i' },
		{ "raw-output", required_argument, NULL, 'r' },
		{ "color-output", required_argument, NULL, 'c' },
		{ "color-width", required_argument, NULL, OPTION_COLOR_WIDTH },
		{ "color-height", required_argument, NULL, OPTION_COLOR_HEIGHT },
		{ "stall-timeout", required_argument, NULL, OPTION_STALL_TIMEOUT },
		{ "frames", required_argument, NULL, 'n' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int option;

	*options = (struct options) {
		.input_path = DEFAULT_INPUT_DEVICE,
		.raw_path = DEFAULT_RAW_DEVICE,
		.color_path = DEFAULT_COLOR_DEVICE,
		.color_width = DEFAULT_COLOR_WIDTH,
		.color_height = DEFAULT_COLOR_HEIGHT,
	};

	while ((option = getopt_long(argc, argv, "i:r:c:n:qh", long_options,
				     NULL)) != -1) {
		switch (option) {
		case 'i':
			options->input_path = optarg;
			break;
		case 'r':
			options->raw_path = optarg;
			break;
		case 'c':
			options->color_path = optarg;
			break;
		case 'n':
			if (parse_u64(optarg, &options->frame_limit) != 0)
				return -1;
			break;
		case 'q':
			options->quiet = true;
			break;
		case OPTION_COLOR_WIDTH:
			if (parse_unsigned(optarg, &options->color_width) != 0)
				return -1;
			break;
		case OPTION_COLOR_HEIGHT:
			if (parse_unsigned(optarg, &options->color_height) != 0)
				return -1;
			break;
		case OPTION_STALL_TIMEOUT:
			if (parse_unsigned(optarg, &options->stall_timeout_seconds) != 0)
				return -1;
			break;
		case 'h':
			print_usage(stdout, argv[0]);
			exit(EXIT_SUCCESS);
		default:
			return -1;
		}
	}

	if (optind != argc || options->color_width == 0 ||
	    options->color_width > MAX_OUTPUT_DIMENSION ||
	    options->color_height == 0 ||
	    options->color_height > MAX_OUTPUT_DIMENSION ||
	    (options->color_width & 1U) != 0 ||
	    strcmp(options->input_path, options->raw_path) == 0 ||
	    strcmp(options->input_path, options->color_path) == 0 ||
	    strcmp(options->raw_path, options->color_path) == 0)
		return -1;

	return 0;
}

static int verify_character_device(const char *path)
{
	struct stat status;

	if (stat(path, &status) != 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return -1;
	}
	if (!S_ISCHR(status.st_mode)) {
		fprintf(stderr, "%s is not a character device\n", path);
		errno = ENODEV;
		return -1;
	}
	return 0;
}

static void close_capture(struct capture_device *capture)
{
	unsigned int index;

	if (capture->fd >= 0 && capture->streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if (xioctl(capture->fd, VIDIOC_STREAMOFF, &type) != 0 &&
		    errno != ENODEV)
			fprintf(stderr, "%s: VIDIOC_STREAMOFF: %s\n",
				capture->path, strerror(errno));
		capture->streaming = false;
	}

	for (index = 0; index < capture->buffer_count; index++) {
		if (capture->buffers[index].address != MAP_FAILED &&
		    capture->buffers[index].address != NULL)
			munmap(capture->buffers[index].address,
			       capture->buffers[index].length);
	}
	free(capture->buffers);
	capture->buffers = NULL;
	capture->buffer_count = 0;

	if (capture->fd >= 0)
		close(capture->fd);
	capture->fd = -1;
}

static int open_capture(struct capture_device *capture, const char *path)
{
	struct v4l2_capability capability = { 0 };
	struct v4l2_format format = { 0 };
	struct v4l2_requestbuffers request = { 0 };
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	uint32_t caps;
	unsigned int index;
	char fourcc[5];

	*capture = (struct capture_device) { .fd = -1, .path = path };
	if (verify_character_device(path) != 0)
		return -1;

	capture->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (capture->fd < 0) {
		fprintf(stderr, "%s: open: %s\n", path, strerror(errno));
		return -1;
	}

	if (xioctl(capture->fd, VIDIOC_QUERYCAP, &capability) != 0) {
		fprintf(stderr, "%s: VIDIOC_QUERYCAP: %s\n", path, strerror(errno));
		goto fail;
	}
	caps = effective_caps(&capability);
	if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
	    (caps & V4L2_CAP_STREAMING) == 0) {
		fprintf(stderr, "%s does not support streaming video capture\n", path);
		errno = ENOTSUP;
		goto fail;
	}

	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = LEPTON_SUBFRAME_LINE_WORD_COUNT;
	format.fmt.pix.height = LEPTON_SUBFRAME_DATA_LINE_HEIGHT;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_Y16;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(capture->fd, VIDIOC_S_FMT, &format) != 0) {
		fprintf(stderr, "%s: VIDIOC_S_FMT: %s\n", path, strerror(errno));
		goto fail;
	}
	if (format.fmt.pix.width != LEPTON_SUBFRAME_LINE_WORD_COUNT ||
	    format.fmt.pix.height != LEPTON_SUBFRAME_DATA_LINE_HEIGHT ||
	    format.fmt.pix.pixelformat != V4L2_PIX_FMT_Y16 ||
	    format.fmt.pix.sizeimage < LEPTON_SUBFRAME_SIZE) {
		fourcc_to_string(format.fmt.pix.pixelformat, fourcc);
		fprintf(stderr,
			"%s returned unsupported input format %ux%u %s, size=%u\n",
			path, format.fmt.pix.width, format.fmt.pix.height, fourcc,
			format.fmt.pix.sizeimage);
		errno = EPROTO;
		goto fail;
	}

	request.count = CAPTURE_BUFFER_COUNT;
	request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	request.memory = V4L2_MEMORY_MMAP;
	if (xioctl(capture->fd, VIDIOC_REQBUFS, &request) != 0) {
		fprintf(stderr, "%s: VIDIOC_REQBUFS: %s\n", path, strerror(errno));
		goto fail;
	}
	if (request.count < 2) {
		fprintf(stderr, "%s returned only %u capture buffer(s)\n",
			path, request.count);
		errno = ENOMEM;
		goto fail;
	}

	capture->buffers = calloc(request.count, sizeof(*capture->buffers));
	if (!capture->buffers)
		goto fail;
	capture->buffer_count = request.count;

	for (index = 0; index < capture->buffer_count; index++) {
		struct v4l2_buffer buffer = { 0 };

		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		if (xioctl(capture->fd, VIDIOC_QUERYBUF, &buffer) != 0) {
			fprintf(stderr, "%s: VIDIOC_QUERYBUF[%u]: %s\n",
				path, index, strerror(errno));
			goto fail;
		}

		capture->buffers[index].length = buffer.length;
		capture->buffers[index].address = mmap(
			NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
			capture->fd, buffer.m.offset);
		if (capture->buffers[index].address == MAP_FAILED) {
			fprintf(stderr, "%s: mmap[%u]: %s\n",
				path, index, strerror(errno));
			goto fail;
		}
	}

	for (index = 0; index < capture->buffer_count; index++) {
		struct v4l2_buffer buffer = { 0 };

		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		if (xioctl(capture->fd, VIDIOC_QBUF, &buffer) != 0) {
			fprintf(stderr, "%s: VIDIOC_QBUF[%u]: %s\n",
				path, index, strerror(errno));
			goto fail;
		}
	}

	if (xioctl(capture->fd, VIDIOC_STREAMON, &type) != 0) {
		fprintf(stderr, "%s: VIDIOC_STREAMON: %s\n", path, strerror(errno));
		goto fail;
	}
	capture->streaming = true;
	fprintf(stderr, "Input: %s (VoSPI 82x60 transport)\n", path);
	return 0;

fail:
	close_capture(capture);
	return -1;
}

static void close_output(struct output_device *output)
{
	unsigned int index;

	if (output->fd >= 0 && output->streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

		if (xioctl(output->fd, VIDIOC_STREAMOFF, &type) != 0 &&
		    errno != ENODEV)
			fprintf(stderr, "%s: VIDIOC_STREAMOFF: %s\n",
				output->path, strerror(errno));
		output->streaming = false;
	}

	for (index = 0; index < output->buffer_count; index++) {
		if (output->buffers[index].address != MAP_FAILED &&
		    output->buffers[index].address != NULL)
			munmap(output->buffers[index].address,
			       output->buffers[index].length);
	}
	free(output->buffers);
	output->buffers = NULL;
	output->buffer_count = 0;

	if (output->fd >= 0) {
		struct v4l2_requestbuffers request = {
			.count = 0,
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			.memory = V4L2_MEMORY_MMAP,
		};

		(void)xioctl(output->fd, VIDIOC_REQBUFS, &request);
	}
	if (output->fd >= 0)
		close(output->fd);
	output->fd = -1;
}

static int verify_capture_view(
	const char *path,
	unsigned int width,
	unsigned int height,
	uint32_t pixel_format)
{
	struct v4l2_capability capability = { 0 };
	struct v4l2_format format = { 0 };
	char fourcc[5];
	int fd;
	int saved_errno;

	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (xioctl(fd, VIDIOC_QUERYCAP, &capability) != 0)
		goto fail;
	if ((effective_caps(&capability) & V4L2_CAP_VIDEO_CAPTURE) == 0) {
		errno = EPROTO;
		goto fail;
	}

	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd, VIDIOC_G_FMT, &format) != 0)
		goto fail;
	if (format.fmt.pix.width != width || format.fmt.pix.height != height ||
	    format.fmt.pix.pixelformat != pixel_format ||
	    format.fmt.pix.sizeimage < width * height * 2U) {
		fourcc_to_string(format.fmt.pix.pixelformat, fourcc);
		fprintf(stderr,
			"%s capture view mismatch: %ux%u %s size=%u\n",
			path, format.fmt.pix.width, format.fmt.pix.height,
			fourcc, format.fmt.pix.sizeimage);
		errno = EPROTO;
		goto fail;
	}

	close(fd);
	return 0;

fail:
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return -1;
}

static int open_output(
	struct output_device *output,
	const char *path,
	unsigned int width,
	unsigned int height,
	uint32_t pixel_format,
	uint32_t colorspace)
{
	struct v4l2_capability capability = { 0 };
	struct v4l2_format format = { 0 };
	struct v4l2_requestbuffers request = { 0 };
	struct v4l2_streamparm stream_parameters = { 0 };
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	uint32_t caps;
	unsigned int index;
	char fourcc[5];

	*output = (struct output_device) { .fd = -1, .path = path };
	if (verify_character_device(path) != 0)
		return -1;

	output->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (output->fd < 0) {
		fprintf(stderr, "%s: open: %s\n", path, strerror(errno));
		return -1;
	}

	if (xioctl(output->fd, VIDIOC_QUERYCAP, &capability) != 0) {
		fprintf(stderr, "%s: VIDIOC_QUERYCAP: %s\n", path, strerror(errno));
		goto fail;
	}
	caps = effective_caps(&capability);
	if ((caps & V4L2_CAP_VIDEO_OUTPUT) == 0 ||
	    (caps & V4L2_CAP_STREAMING) == 0) {
		fprintf(stderr, "%s does not support streaming video output\n", path);
		errno = ENOTSUP;
		goto fail;
	}

	format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	format.fmt.pix.width = width;
	format.fmt.pix.height = height;
	format.fmt.pix.pixelformat = pixel_format;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	format.fmt.pix.colorspace = colorspace;
	format.fmt.pix.bytesperline = width * 2U;
	format.fmt.pix.sizeimage = width * height * 2U;
	if (pixel_format == V4L2_PIX_FMT_YUYV) {
		format.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_601;
		format.fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
		format.fmt.pix.xfer_func = V4L2_XFER_FUNC_SRGB;
	}

	if (xioctl(output->fd, VIDIOC_S_FMT, &format) != 0) {
		fprintf(stderr, "%s: VIDIOC_S_FMT: %s\n", path, strerror(errno));
		goto fail;
	}
	if (format.fmt.pix.width != width || format.fmt.pix.height != height ||
	    format.fmt.pix.pixelformat != pixel_format ||
	    format.fmt.pix.sizeimage < width * height * 2U) {
		fourcc_to_string(format.fmt.pix.pixelformat, fourcc);
		fprintf(stderr,
			"%s refused output format %ux%u; returned %ux%u %s size=%u\n",
			path, width, height, format.fmt.pix.width,
			format.fmt.pix.height, fourcc, format.fmt.pix.sizeimage);
		errno = EPROTO;
		goto fail;
	}
	output->frame_size = width * height * 2U;

	stream_parameters.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	stream_parameters.parm.output.timeperframe.numerator = 1;
	stream_parameters.parm.output.timeperframe.denominator =
		OUTPUT_FRAMES_PER_SECOND;
	if (xioctl(output->fd, VIDIOC_S_PARM, &stream_parameters) != 0 &&
	    errno != EINVAL && errno != ENOTTY) {
		fprintf(stderr, "%s: VIDIOC_S_PARM: %s\n", path, strerror(errno));
		goto fail;
	}

	request.count = OUTPUT_BUFFER_COUNT;
	request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	request.memory = V4L2_MEMORY_MMAP;
	if (xioctl(output->fd, VIDIOC_REQBUFS, &request) != 0) {
		fprintf(stderr, "%s: VIDIOC_REQBUFS: %s\n", path, strerror(errno));
		goto fail;
	}
	if (request.count < 2) {
		fprintf(stderr, "%s returned only %u output buffer(s)\n",
			path, request.count);
		errno = ENOMEM;
		goto fail;
	}

	output->buffers = calloc(request.count, sizeof(*output->buffers));
	if (!output->buffers)
		goto fail;
	output->buffer_count = request.count;

	for (index = 0; index < output->buffer_count; index++) {
		struct v4l2_buffer buffer = { 0 };

		buffer.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = index;
		if (xioctl(output->fd, VIDIOC_QUERYBUF, &buffer) != 0) {
			fprintf(stderr, "%s: VIDIOC_QUERYBUF[%u]: %s\n",
				path, index, strerror(errno));
			goto fail;
		}
		if (buffer.length < output->frame_size) {
			fprintf(stderr,
				"%s output buffer %u is too small: %u < %zu bytes\n",
				path, index, buffer.length, output->frame_size);
			errno = EPROTO;
			goto fail;
		}

		output->buffers[index].length = buffer.length;
		output->buffers[index].address = mmap(
			NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
			output->fd, buffer.m.offset);
		if (output->buffers[index].address == MAP_FAILED) {
			fprintf(stderr, "%s: mmap[%u]: %s\n",
				path, index, strerror(errno));
			goto fail;
		}
	}

	/* Claim the output stream before queueing data so capture clients can
	 * negotiate immediately, while invalid VoSPI input still emits no frame. */
	if (xioctl(output->fd, VIDIOC_STREAMON, &type) != 0) {
		fprintf(stderr, "%s: VIDIOC_STREAMON: %s\n", path, strerror(errno));
		goto fail;
	}
	output->streaming = true;

	if (verify_capture_view(path, width, height, pixel_format) != 0) {
		fprintf(stderr, "%s is not usable as a capture device: %s\n",
			path, strerror(errno));
		goto fail;
	}

	fourcc_to_string(pixel_format, fourcc);
	fprintf(stderr, "Output: %s (%ux%u %s)\n", path, width, height, fourcc);
	return 0;

fail:
	close_output(output);
	return -1;
}

static int write_output_frame(
	const struct output_device *output,
	const void *frame,
	size_t frame_size)
{
	unsigned int attempt;
	struct v4l2_buffer buffer = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
		.memory = V4L2_MEMORY_MMAP,
	};

	if (!output->streaming || output->buffer_count == 0 ||
	    frame_size != output->frame_size) {
		errno = EINVAL;
		return -1;
	}

	for (attempt = 0; attempt < 3; attempt++) {
		if (xioctl(output->fd, VIDIOC_DQBUF, &buffer) == 0)
			break;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			struct pollfd poll_fd = {
				.fd = output->fd,
				.events = POLLOUT,
			};
			int poll_result = poll(&poll_fd, 1, OUTPUT_POLL_TIMEOUT_MS);

			if (poll_result > 0)
				continue;
			if (poll_result == 0)
				errno = ETIMEDOUT;
			return -1;
		}
		return -1;
	}

	if (attempt == 3) {
		errno = EAGAIN;
		return -1;
	}
	if (buffer.index >= output->buffer_count ||
	    output->buffers[buffer.index].length < frame_size) {
		errno = EPROTO;
		return -1;
	}

	memcpy(output->buffers[buffer.index].address, frame, frame_size);
	buffer.bytesused = (uint32_t)frame_size;
	buffer.field = V4L2_FIELD_NONE;
	buffer.timestamp.tv_sec = 0;
	buffer.timestamp.tv_usec = 0;
	buffer.flags &= ~V4L2_BUF_FLAG_TIMESTAMP_COPY;
	if (xioctl(output->fd, VIDIOC_QBUF, &buffer) != 0)
		return -1;

	return 0;
}

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void log_rejected_subframe(
	const struct lepton_frame_assembler *assembler,
	const uint8_t *subframe,
	size_t size)
{
	uint64_t count = assembler->rejected_subframes;

	if (count > 8U && count % 256U != 0)
		return;

	if (size >= LEPTON_SUBFRAME_SIZE) {
		const uint8_t *line20 = subframe +
			LEPTON3_SUBFRAME_INDEX_LINE1 *
			LEPTON_SUBFRAME_LINE_BYTE_WIDTH;
		fprintf(stderr,
			"Rejected VoSPI subframe #%llu: %s, l0=%02x%02x l20=%02x%02x\n",
			(unsigned long long)count,
			lepton_reject_reason_name(assembler->last_reject),
			subframe[0], subframe[1], line20[0], line20[1]);
	} else {
		fprintf(stderr, "Rejected VoSPI subframe #%llu: %s, size=%zu\n",
			(unsigned long long)count,
			lepton_reject_reason_name(assembler->last_reject), size);
	}
}

static int process_capture_buffer(
	struct capture_device *capture,
	const struct v4l2_buffer *buffer,
	struct lepton_frame_assembler *assembler,
	uint16_t raw_frame[LEPTON_FRAME_PIXELS],
	uint8_t raw_y16le[LEPTON_Y16_FRAME_BYTES],
	uint8_t *color_yuyv,
	const struct options *options,
	const struct output_device *raw_output,
	const struct output_device *color_output,
	struct lepton_frame_range *range,
	uint64_t *published_frames)
{
	const uint8_t *subframe;
	size_t bytes_used;
	enum lepton_assemble_result result;

	if (buffer->index >= capture->buffer_count) {
		fprintf(stderr, "%s returned invalid buffer index %u\n",
			capture->path, buffer->index);
		errno = EPROTO;
		return -1;
	}

	subframe = capture->buffers[buffer->index].address;
	bytes_used = buffer->bytesused;
	if ((buffer->flags & V4L2_BUF_FLAG_ERROR) != 0) {
		assembler->vospi.next_subframe_index = 1;
		assembler->rejected_subframes++;
		assembler->last_reject = LEPTON_REJECT_EXTRACTION;
		log_rejected_subframe(assembler, subframe, bytes_used);
		return 0;
	}

	result = lepton_frame_assembler_push(
		assembler, subframe, bytes_used, raw_frame);
	if (result == LEPTON_ASSEMBLE_ERROR) {
		errno = EINVAL;
		return -1;
	}
	if (result == LEPTON_ASSEMBLE_REJECTED) {
		log_rejected_subframe(assembler, subframe, bytes_used);
		return 0;
	}
	if (result != LEPTON_ASSEMBLE_FRAME_READY)
		return 0;

	lepton_frame_to_y16le(raw_frame, raw_y16le);
	if (lepton_render_false_color_yuyv(
			raw_frame, color_yuyv, options->color_width,
			options->color_height, range) != 0) {
		fprintf(stderr, "Unable to render false-color frame\n");
		return -1;
	}

	if (write_output_frame(raw_output, raw_y16le,
			       LEPTON_Y16_FRAME_BYTES) != 0) {
		fprintf(stderr, "%s: write: %s\n",
			raw_output->path, strerror(errno));
		return -1;
	}
	if (write_output_frame(color_output, color_yuyv,
			       color_output->frame_size) != 0) {
		fprintf(stderr, "%s: write: %s\n",
			color_output->path, strerror(errno));
		return -1;
	}

	(*published_frames)++;
	return 0;
}

static int run_streamer(const struct options *options)
{
	struct capture_device capture = { .fd = -1 };
	struct output_device raw_output = { .fd = -1 };
	struct output_device color_output = { .fd = -1 };
	struct lepton_frame_assembler assembler;
	struct lepton_frame_range range = { 0 };
	uint16_t *raw_frame = NULL;
	uint8_t *raw_y16le = NULL;
	uint8_t *color_yuyv = NULL;
	size_t color_size;
	uint64_t published_frames = 0;
	uint64_t last_frame_ms;
	uint64_t last_status_ms;
	int return_code = STREAMER_ERROR;

	if (options->color_width > SIZE_MAX / options->color_height / 2U ||
	    options->color_width > UINT32_MAX / options->color_height / 2U) {
		fprintf(stderr, "False-color dimensions are too large\n");
		return -1;
	}
	color_size = (size_t)options->color_width * options->color_height * 2U;

	raw_frame = calloc(LEPTON_FRAME_PIXELS, sizeof(*raw_frame));
	raw_y16le = malloc(LEPTON_Y16_FRAME_BYTES);
	color_yuyv = malloc(color_size);
	if (!raw_frame || !raw_y16le || !color_yuyv) {
		fprintf(stderr, "Unable to allocate stream buffers\n");
		goto done;
	}

	lepton_frame_assembler_init(&assembler);
	if (open_output(&raw_output, options->raw_path,
			LEPTON_FRAME_WIDTH, LEPTON_FRAME_HEIGHT,
			V4L2_PIX_FMT_Y16, V4L2_COLORSPACE_RAW) != 0)
		goto done;
	if (open_output(&color_output, options->color_path,
			options->color_width, options->color_height,
			V4L2_PIX_FMT_YUYV, V4L2_COLORSPACE_SRGB) != 0)
		goto done;
	if (open_capture(&capture, options->input_path) != 0)
		goto done;

	fprintf(stderr,
		"Lepton streams running: raw=%s, false-color=%s\n",
		options->raw_path, options->color_path);
	last_status_ms = monotonic_milliseconds();
	last_frame_ms = last_status_ms;

	while (!stop_requested &&
	       (options->frame_limit == 0 ||
		published_frames < options->frame_limit)) {
		struct pollfd poll_fd = {
			.fd = capture.fd,
			.events = POLLIN | POLLPRI,
		};
		struct v4l2_buffer buffer = { 0 };
		uint64_t frames_before = published_frames;
		uint64_t now_ms;
		int poll_result = poll(&poll_fd, 1, INPUT_POLL_TIMEOUT_MS);

		if (poll_result < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "%s: poll: %s\n",
				capture.path, strerror(errno));
			goto done;
		}
		if (poll_result == 0) {
			fprintf(stderr, "%s: no VoSPI buffer for %d ms\n",
				capture.path, INPUT_POLL_TIMEOUT_MS);
			now_ms = monotonic_milliseconds();
			if (options->stall_timeout_seconds != 0 &&
			    now_ms - last_frame_ms >=
				    (uint64_t)options->stall_timeout_seconds * 1000U) {
				fprintf(stderr,
					"No complete Lepton frame for %u seconds; requesting recovery\n",
					options->stall_timeout_seconds);
				return_code = STREAMER_STALLED;
				goto done;
			}
			continue;
		}
		if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			fprintf(stderr, "%s: capture device disconnected (revents=0x%x)\n",
				capture.path,
				(unsigned int)(unsigned short)poll_fd.revents);
			errno = ENODEV;
			goto done;
		}

		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		if (xioctl(capture.fd, VIDIOC_DQBUF, &buffer) != 0) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "%s: VIDIOC_DQBUF: %s\n",
				capture.path, strerror(errno));
			goto done;
		}

		if (process_capture_buffer(
				&capture, &buffer, &assembler, raw_frame, raw_y16le,
				color_yuyv, options, &raw_output, &color_output,
				&range, &published_frames) != 0) {
			(void)xioctl(capture.fd, VIDIOC_QBUF, &buffer);
			goto done;
		}
		if (xioctl(capture.fd, VIDIOC_QBUF, &buffer) != 0) {
			fprintf(stderr, "%s: VIDIOC_QBUF: %s\n",
				capture.path, strerror(errno));
			goto done;
		}

		now_ms = monotonic_milliseconds();
		if (published_frames != frames_before)
			last_frame_ms = now_ms;
		else if (options->stall_timeout_seconds != 0 &&
			 now_ms - last_frame_ms >=
				 (uint64_t)options->stall_timeout_seconds * 1000U) {
			fprintf(stderr,
				"No complete Lepton frame for %u seconds; requesting recovery\n",
				options->stall_timeout_seconds);
			return_code = STREAMER_STALLED;
			goto done;
		}

		if (!options->quiet &&
		    now_ms - last_status_ms >= 2000U) {
			fprintf(stderr,
				"frames=%llu accepted=%llu skipped=%llu rejected=%llu min=%u max=%u\n",
				(unsigned long long)published_frames,
				(unsigned long long)assembler.accepted_subframes,
				(unsigned long long)assembler.skipped_subframes,
				(unsigned long long)assembler.rejected_subframes,
				range.minimum, range.maximum);
			last_status_ms = now_ms;
		}
	}

	return_code = STREAMER_OK;

done:
	close_capture(&capture);
	close_output(&color_output);
	close_output(&raw_output);
	free(color_yuyv);
	free(raw_y16le);
	free(raw_frame);
	return return_code;
}

int main(int argc, char **argv)
{
	struct sigaction action = { 0 };
	struct options options;

	if (parse_options(argc, argv, &options) != 0) {
		print_usage(stderr, argv[0]);
		return EXIT_FAILURE;
	}

	action.sa_handler = handle_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) != 0 ||
	    sigaction(SIGTERM, &action, NULL) != 0) {
		fprintf(stderr, "sigaction: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	{
		int result = run_streamer(&options);

		if (result == STREAMER_OK)
			return EXIT_SUCCESS;
		if (result == STREAMER_STALLED)
			return STALL_RECOVERY_EXIT_STATUS;
		return EXIT_FAILURE;
	}
}
