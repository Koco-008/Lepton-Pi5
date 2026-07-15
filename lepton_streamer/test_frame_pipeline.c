#include "frame_pipeline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_BASE_VALUE 1000U

static int failures;

#define CHECK(condition)                                                        \
	do {                                                                      \
		if (!(condition)) {                                                \
			fprintf(stderr, "%s:%d: check failed: %s\n",               \
				__FILE__, __LINE__, #condition);                      \
			failures++;                                                   \
		}                                                                 \
	} while (0)

static void make_subframe(unsigned int segment, uint8_t *subframe)
{
	int line_index;

	memset(subframe, 0, LEPTON_SUBFRAME_SIZE);
	for (line_index = 0; line_index < LEPTON_SUBFRAME_DATA_LINE_HEIGHT;
	     line_index++) {
		uint8_t *line = subframe +
			line_index * LEPTON_SUBFRAME_LINE_BYTE_WIDTH;
		int pixel_index;

		line[1] = (uint8_t)line_index;
		if (line_index == LEPTON3_SUBFRAME_INDEX_LINE1)
			line[0] |= (uint8_t)((segment & 0x07U) << 4);
		if (line_index == LEPTON3_SUBFRAME_INDEX_LINE2)
			line[0] |= (uint8_t)((segment & 0x08U) << 1);

		for (pixel_index = 0; pixel_index < LEPTON_SUBFRAME_LINE_PIXEL_WIDTH;
		     pixel_index++) {
			unsigned int image_segment = segment == 0 ? 0 : segment - 1U;
			unsigned int logical_index =
				image_segment *
					(LEPTON_SUBFRAME_DATA_LINE_HEIGHT / 2U) *
					LEPTON_FRAME_WIDTH +
				(unsigned int)line_index *
					LEPTON_SUBFRAME_LINE_PIXEL_WIDTH +
				(unsigned int)pixel_index;
			uint16_t value = (uint16_t)(TEST_BASE_VALUE + logical_index);
			unsigned int byte_offset = 4U + pixel_index * 2U;

			line[byte_offset] = (uint8_t)(value >> 8);
			line[byte_offset + 1U] = (uint8_t)(value & 0xffU);
		}
	}
}

static void test_complete_frame(void)
{
	struct lepton_frame_assembler assembler;
	uint8_t subframe[LEPTON_SUBFRAME_SIZE];
	uint16_t frame[LEPTON_FRAME_PIXELS];
	unsigned int segment;
	unsigned int pixel_index;

	lepton_frame_assembler_init(&assembler);
	for (segment = 1; segment <= LEPTON3_SUBFRAME_COUNT; segment++) {
		enum lepton_assemble_result result;

		make_subframe(segment, subframe);
		result = lepton_frame_assembler_push(
			&assembler, subframe, sizeof(subframe), frame);
		CHECK(result == (segment == LEPTON3_SUBFRAME_COUNT ?
			LEPTON_ASSEMBLE_FRAME_READY : LEPTON_ASSEMBLE_ACCEPTED));
	}

	for (pixel_index = 0; pixel_index < LEPTON_FRAME_PIXELS; pixel_index++)
		CHECK((unsigned int)frame[pixel_index] ==
		      TEST_BASE_VALUE + pixel_index);
	CHECK(assembler.accepted_subframes == 4);
	CHECK(assembler.rejected_subframes == 0);
	CHECK(assembler.completed_frames == 1);
}

static void test_invalid_frame_cadence(void)
{
	struct lepton_frame_assembler assembler;
	uint8_t subframe[LEPTON_SUBFRAME_SIZE];
	uint16_t frame[LEPTON_FRAME_PIXELS];
	unsigned int segment;
	unsigned int zero_segment;

	lepton_frame_assembler_init(&assembler);
	for (segment = 1; segment <= LEPTON3_SUBFRAME_COUNT; segment++) {
		make_subframe(segment, subframe);
		CHECK(lepton_frame_assembler_push(
			&assembler, subframe, sizeof(subframe), frame) ==
			(segment == LEPTON3_SUBFRAME_COUNT ?
			 LEPTON_ASSEMBLE_FRAME_READY : LEPTON_ASSEMBLE_ACCEPTED));
	}
	for (zero_segment = 0; zero_segment < 8; zero_segment++) {
		make_subframe(0, subframe);
		CHECK(lepton_frame_assembler_push(
			&assembler, subframe, sizeof(subframe), frame) ==
			LEPTON_ASSEMBLE_SKIPPED);
	}
	for (segment = 1; segment <= LEPTON3_SUBFRAME_COUNT; segment++) {
		make_subframe(segment, subframe);
		CHECK(lepton_frame_assembler_push(
			&assembler, subframe, sizeof(subframe), frame) ==
			(segment == LEPTON3_SUBFRAME_COUNT ?
			 LEPTON_ASSEMBLE_FRAME_READY : LEPTON_ASSEMBLE_ACCEPTED));
	}

	CHECK(assembler.completed_frames == 2);
	CHECK(assembler.accepted_subframes == 8);
	CHECK(assembler.skipped_subframes == 8);
	CHECK(assembler.rejected_subframes == 0);
	CHECK(assembler.vospi.next_subframe_index == 1);
}

static void test_rejection_and_resync(void)
{
	struct lepton_frame_assembler assembler;
	uint8_t subframe[LEPTON_SUBFRAME_SIZE];
	uint16_t frame[LEPTON_FRAME_PIXELS];
	unsigned int segment;

	lepton_frame_assembler_init(&assembler);
	make_subframe(2, subframe);
	CHECK(lepton_frame_assembler_push(&assembler, subframe, sizeof(subframe),
					   frame) == LEPTON_ASSEMBLE_REJECTED);
	CHECK(assembler.last_reject == LEPTON_REJECT_SEGMENT);

	make_subframe(1, subframe);
	CHECK(lepton_frame_assembler_push(&assembler, subframe, sizeof(subframe),
					   frame) == LEPTON_ASSEMBLE_ACCEPTED);
	make_subframe(3, subframe);
	CHECK(lepton_frame_assembler_push(&assembler, subframe, sizeof(subframe),
					   frame) == LEPTON_ASSEMBLE_REJECTED);
	CHECK(assembler.vospi.next_subframe_index == 1);

	for (segment = 1; segment <= LEPTON3_SUBFRAME_COUNT; segment++) {
		make_subframe(segment, subframe);
		CHECK(lepton_frame_assembler_push(
			&assembler, subframe, sizeof(subframe), frame) ==
			(segment == LEPTON3_SUBFRAME_COUNT ?
			 LEPTON_ASSEMBLE_FRAME_READY : LEPTON_ASSEMBLE_ACCEPTED));
	}

	make_subframe(1, subframe);
	subframe[0] = 0x0f;
	CHECK(lepton_frame_assembler_push(&assembler, subframe, sizeof(subframe),
					   frame) == LEPTON_ASSEMBLE_REJECTED);
	CHECK(assembler.last_reject == LEPTON_REJECT_DISCARD);

	make_subframe(1, subframe);
	subframe[LEPTON_SUBFRAME_LINE_BYTE_WIDTH + 1U] = 7;
	CHECK(lepton_frame_assembler_push(&assembler, subframe, sizeof(subframe),
					   frame) == LEPTON_ASSEMBLE_REJECTED);
	CHECK(assembler.last_reject == LEPTON_REJECT_PACKET_ID);
}

static void test_palette_and_serialization(void)
{
	struct lepton_frame_range range = { .minimum = 100, .maximum = 200 };
	uint16_t frame[LEPTON_FRAME_PIXELS];
	uint8_t y16le[LEPTON_Y16_FRAME_BYTES];
	uint8_t yuyv[4U * 2U * 2U];
	uint8_t red, green, blue;
	unsigned int x;
	unsigned int y;

	lepton_false_color_rgb(100, &range, &red, &green, &blue);
	CHECK(red == 0 && green == 0 && blue == 255);
	lepton_false_color_rgb(200, &range, &red, &green, &blue);
	CHECK(red == 255 && green == 0 && blue == 0);

	for (y = 0; y < LEPTON_FRAME_HEIGHT; y++) {
		for (x = 0; x < LEPTON_FRAME_WIDTH; x++)
			frame[y * LEPTON_FRAME_WIDTH + x] = x < 80U ? 100 : 200;
	}
	lepton_find_frame_range(frame, &range);
	CHECK(range.minimum == 100);
	CHECK(range.maximum == 200);

	CHECK(lepton_render_false_color_yuyv(frame, yuyv, 4, 2, &range) == 0);
	CHECK(range.minimum == 100 && range.maximum == 200);
	CHECK(yuyv[0] == 41 && yuyv[1] == 240 &&
	      yuyv[2] == 41 && yuyv[3] == 110);
	CHECK(yuyv[4] == 82 && yuyv[5] == 90 &&
	      yuyv[6] == 82 && yuyv[7] == 240);
	CHECK(lepton_render_false_color_yuyv(frame, yuyv, 3, 2, NULL) < 0);

	frame[0] = 0x1234;
	lepton_frame_to_y16le(frame, y16le);
	CHECK(y16le[0] == 0x34 && y16le[1] == 0x12);
}

int main(void)
{
	test_complete_frame();
	test_invalid_frame_cadence();
	test_rejection_and_resync();
	test_palette_and_serialization();

	if (failures != 0) {
		fprintf(stderr, "%d frame-pipeline test(s) failed\n", failures);
		return EXIT_FAILURE;
	}

	puts("frame-pipeline tests passed");
	return EXIT_SUCCESS;
}
