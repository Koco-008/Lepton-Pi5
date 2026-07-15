#include "frame_pipeline.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static int packet_ids_are_valid(const uint8_t *subframe)
{
	int line_index;

	for (line_index = 0; line_index < LEPTON_SUBFRAME_DATA_LINE_HEIGHT;
	     line_index++) {
		const uint8_t *line = subframe +
			line_index * LEPTON_SUBFRAME_LINE_BYTE_WIDTH;

		/* The low 12 bits are the packet ID. Valid image packets here
		 * are numbered 0..59; segment bits occupy the upper nibble.
		 */
		if ((line[0] & 0x0fU) != 0 || line[1] != (uint8_t)line_index)
			return 0;
	}

	return 1;
}

static void reset_segment_sequence(struct lepton_frame_assembler *assembler)
{
	assembler->vospi.next_subframe_index = 1;
}

void lepton_frame_assembler_init(struct lepton_frame_assembler *assembler)
{
	memset(assembler, 0, sizeof(*assembler));
	init_lepton_info(&assembler->vospi, LEPTON_VERSION_3X, TELEMETRY_OFF);
}

enum lepton_assemble_result lepton_frame_assembler_push(
	struct lepton_frame_assembler *assembler,
	const void *subframe,
	size_t subframe_size,
	uint16_t output_frame[LEPTON_FRAME_PIXELS])
{
	const uint8_t *bytes = subframe;
	const uint8_t *wire_pixels;
	int done = 0;
	int extraction_errors;
	unsigned int pixel_index;

	if (!assembler || !subframe || !output_frame)
		return LEPTON_ASSEMBLE_ERROR;

	assembler->last_reject = LEPTON_REJECT_NONE;

	if (subframe_size < LEPTON_SUBFRAME_SIZE) {
		assembler->last_reject = LEPTON_REJECT_SIZE;
		goto reject;
	}

	if ((bytes[0] & 0x0fU) == 0x0fU) {
		assembler->last_reject = LEPTON_REJECT_DISCARD;
		goto reject;
	}

	if (!packet_ids_are_valid(bytes)) {
		assembler->last_reject = LEPTON_REJECT_PACKET_ID;
		goto reject;
	}

	if (lepton_get_subframe_index(
			&assembler->vospi,
			(unsigned short *)(uintptr_t)subframe) == 0) {
		/* Lepton 3 emits eight zero-numbered segments after every unique
		 * four-segment frame. They are protocol cadence, not sync errors.
		 */
		reset_segment_sequence(assembler);
		assembler->skipped_subframes++;
		return LEPTON_ASSEMBLE_SKIPPED;
	}

	if (!is_subframe_index_valid(&assembler->vospi,
					     (unsigned short *)(uintptr_t)subframe)) {
		assembler->last_reject = LEPTON_REJECT_SEGMENT;
		goto reject_without_reset;
	}

	extraction_errors = extract_pixel_data(
		&assembler->vospi,
		(unsigned short *)(uintptr_t)subframe,
		(unsigned short *)(void *)assembler->wire_pixels,
		&done);
	if (extraction_errors != 0) {
		assembler->last_reject = LEPTON_REJECT_EXTRACTION;
		goto reject;
	}

	assembler->accepted_subframes++;
	if (!done)
		return LEPTON_ASSEMBLE_ACCEPTED;

	/* VoSPI transmits each 16-bit pixel most-significant byte first. V4L2
	 * Y16 uses native little-endian samples on the Raspberry Pi, so decode
	 * explicitly instead of reinterpreting the SPI byte stream.
	 */
	wire_pixels = (const uint8_t *)(const void *)assembler->wire_pixels;
	for (pixel_index = 0; pixel_index < LEPTON_FRAME_PIXELS; pixel_index++) {
		output_frame[pixel_index] =
			((uint16_t)wire_pixels[pixel_index * 2U] << 8) |
			wire_pixels[pixel_index * 2U + 1U];
	}

	assembler->completed_frames++;
	return LEPTON_ASSEMBLE_FRAME_READY;

reject:
	reset_segment_sequence(assembler);
reject_without_reset:
	assembler->rejected_subframes++;
	return LEPTON_ASSEMBLE_REJECTED;
}

const char *lepton_reject_reason_name(enum lepton_reject_reason reason)
{
	switch (reason) {
	case LEPTON_REJECT_NONE:
		return "none";
	case LEPTON_REJECT_SIZE:
		return "size";
	case LEPTON_REJECT_DISCARD:
		return "discard";
	case LEPTON_REJECT_PACKET_ID:
		return "packet-id";
	case LEPTON_REJECT_SEGMENT:
		return "segment-order";
	case LEPTON_REJECT_EXTRACTION:
		return "extraction";
	default:
		return "unknown";
	}
}

void lepton_find_frame_range(
	const uint16_t frame[LEPTON_FRAME_PIXELS],
	struct lepton_frame_range *range)
{
	uint16_t minimum = UINT16_MAX;
	uint16_t maximum = 0;
	unsigned int index;

	for (index = 0; index < LEPTON_FRAME_PIXELS; index++) {
		if (frame[index] < minimum)
			minimum = frame[index];
		if (frame[index] > maximum)
			maximum = frame[index];
	}

	range->minimum = minimum;
	range->maximum = maximum;
}

void lepton_false_color_rgb(
	uint16_t value,
	const struct lepton_frame_range *range,
	uint8_t *red,
	uint8_t *green,
	uint8_t *blue)
{
	uint32_t level;
	uint32_t span;

	if (range->maximum == range->minimum) {
		level = 128;
	} else if (value <= range->minimum) {
		level = 0;
	} else if (value >= range->maximum) {
		level = 255;
	} else {
		span = (uint32_t)range->maximum - range->minimum;
		level = ((uint32_t)(value - range->minimum) * 255U + span / 2U) /
			span;
	}

	/* Blue -> cyan -> green -> yellow -> red. */
	if (level < 64U) {
		*red = 0;
		*green = (uint8_t)(level * 255U / 63U);
		*blue = 255;
	} else if (level < 128U) {
		*red = 0;
		*green = 255;
		*blue = (uint8_t)(255U - (level - 64U) * 255U / 63U);
	} else if (level < 192U) {
		*red = (uint8_t)((level - 128U) * 255U / 63U);
		*green = 255;
		*blue = 0;
	} else {
		*red = 255;
		*green = (uint8_t)(255U - (level - 192U) * 255U / 63U);
		*blue = 0;
	}
}

static uint8_t clamp_byte(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return (uint8_t)value;
}

static void rgb_to_yuv(
	uint8_t red,
	uint8_t green,
	uint8_t blue,
	uint8_t *luma,
	uint8_t *chroma_u,
	uint8_t *chroma_v)
{
	int y = ((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16;
	int u = ((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128;
	int v = ((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128;

	*luma = clamp_byte(y);
	*chroma_u = clamp_byte(u);
	*chroma_v = clamp_byte(v);
}

int lepton_render_false_color_yuyv(
	const uint16_t frame[LEPTON_FRAME_PIXELS],
	uint8_t *output,
	unsigned int output_width,
	unsigned int output_height,
	struct lepton_frame_range *range)
{
	struct lepton_frame_range local_range;
	unsigned int output_y;

	if (!frame || !output || output_width == 0 || output_height == 0 ||
	    (output_width & 1U) != 0)
		return -EINVAL;

	lepton_find_frame_range(frame, &local_range);
	if (range)
		*range = local_range;

	for (output_y = 0; output_y < output_height; output_y++) {
		unsigned int source_y =
			(output_y * LEPTON_FRAME_HEIGHT) / output_height;
		unsigned int output_x;

		for (output_x = 0; output_x < output_width; output_x += 2U) {
			unsigned int source_x0 =
				(output_x * LEPTON_FRAME_WIDTH) / output_width;
			unsigned int source_x1 =
				((output_x + 1U) * LEPTON_FRAME_WIDTH) /
				output_width;
			uint16_t value0 = frame[source_y * LEPTON_FRAME_WIDTH +
						source_x0];
			uint16_t value1 = frame[source_y * LEPTON_FRAME_WIDTH +
						source_x1];
			uint8_t red0, green0, blue0;
			uint8_t red1, green1, blue1;
			uint8_t y0, u0, v0;
			uint8_t y1, u1, v1;
			size_t offset =
				((size_t)output_y * output_width + output_x) * 2U;

			lepton_false_color_rgb(value0, &local_range,
					       &red0, &green0, &blue0);
			lepton_false_color_rgb(value1, &local_range,
					       &red1, &green1, &blue1);
			rgb_to_yuv(red0, green0, blue0, &y0, &u0, &v0);
			rgb_to_yuv(red1, green1, blue1, &y1, &u1, &v1);

			output[offset] = y0;
			output[offset + 1U] = (uint8_t)(((unsigned int)u0 + u1) / 2U);
			output[offset + 2U] = y1;
			output[offset + 3U] = (uint8_t)(((unsigned int)v0 + v1) / 2U);
		}
	}

	return 0;
}

void lepton_frame_to_y16le(
	const uint16_t frame[LEPTON_FRAME_PIXELS],
	uint8_t output[LEPTON_Y16_FRAME_BYTES])
{
	unsigned int index;

	for (index = 0; index < LEPTON_FRAME_PIXELS; index++) {
		output[index * 2U] = (uint8_t)(frame[index] & 0xffU);
		output[index * 2U + 1U] = (uint8_t)(frame[index] >> 8);
	}
}
