#ifndef LEPTON_FRAME_PIPELINE_H
#define LEPTON_FRAME_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#include "lepton_vospi_funcs.h"

#define LEPTON_FRAME_WIDTH 160U
#define LEPTON_FRAME_HEIGHT 120U
#define LEPTON_FRAME_PIXELS (LEPTON_FRAME_WIDTH * LEPTON_FRAME_HEIGHT)
#define LEPTON_Y16_FRAME_BYTES (LEPTON_FRAME_PIXELS * 2U)

enum lepton_assemble_result {
	LEPTON_ASSEMBLE_ERROR = -1,
	LEPTON_ASSEMBLE_REJECTED = 0,
	LEPTON_ASSEMBLE_ACCEPTED = 1,
	LEPTON_ASSEMBLE_FRAME_READY = 2,
	LEPTON_ASSEMBLE_SKIPPED = 3,
};

enum lepton_reject_reason {
	LEPTON_REJECT_NONE = 0,
	LEPTON_REJECT_SIZE,
	LEPTON_REJECT_DISCARD,
	LEPTON_REJECT_PACKET_ID,
	LEPTON_REJECT_SEGMENT,
	LEPTON_REJECT_EXTRACTION,
};

struct lepton_frame_assembler {
	lepton_vospi_info vospi;
	uint16_t wire_pixels[LEPTON_FRAME_PIXELS];
	uint64_t accepted_subframes;
	uint64_t skipped_subframes;
	uint64_t rejected_subframes;
	uint64_t completed_frames;
	enum lepton_reject_reason last_reject;
};

struct lepton_frame_range {
	uint16_t minimum;
	uint16_t maximum;
};

void lepton_frame_assembler_init(struct lepton_frame_assembler *assembler);

enum lepton_assemble_result lepton_frame_assembler_push(
	struct lepton_frame_assembler *assembler,
	const void *subframe,
	size_t subframe_size,
	uint16_t output_frame[LEPTON_FRAME_PIXELS]);

const char *lepton_reject_reason_name(enum lepton_reject_reason reason);

void lepton_find_frame_range(
	const uint16_t frame[LEPTON_FRAME_PIXELS],
	struct lepton_frame_range *range);

void lepton_false_color_rgb(
	uint16_t value,
	const struct lepton_frame_range *range,
	uint8_t *red,
	uint8_t *green,
	uint8_t *blue);

int lepton_render_false_color_yuyv(
	const uint16_t frame[LEPTON_FRAME_PIXELS],
	uint8_t *output,
	unsigned int output_width,
	unsigned int output_height,
	struct lepton_frame_range *range);

void lepton_frame_to_y16le(
	const uint16_t frame[LEPTON_FRAME_PIXELS],
	uint8_t output[LEPTON_Y16_FRAME_BYTES]);

#endif
