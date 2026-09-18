#include "lepton_vospi_funcs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(expr) do { \
	if (!(expr)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		failures++; \
	} \
} while (0)

static void make_subframe(uint8_t *buffer, unsigned int segment)
{
	unsigned int line;
	memset(buffer, 0, LEPTON_SUBFRAME_DATA_LINE_HEIGHT *
			LEPTON_SUBFRAME_LINE_BYTE_WIDTH);
	for (line = 0; line < LEPTON_SUBFRAME_DATA_LINE_HEIGHT; line++) {
		uint8_t *packet = buffer + line * LEPTON_SUBFRAME_LINE_BYTE_WIDTH;
		packet[0] = 0;
		packet[1] = (uint8_t)line;
	}
	buffer[LEPTON3_SUBFRAME_INDEX_LINE1 * LEPTON_SUBFRAME_LINE_BYTE_WIDTH] |=
		(uint8_t)((segment & 0x07U) << 4);
	buffer[LEPTON3_SUBFRAME_INDEX_LINE2 * LEPTON_SUBFRAME_LINE_BYTE_WIDTH] |=
		(uint8_t)((segment & 0x08U) << 1);
}

int main(void)
{
	lepton_vospi_info info;
	uint8_t subframe[LEPTON_SUBFRAME_DATA_LINE_HEIGHT *
			 LEPTON_SUBFRAME_LINE_BYTE_WIDTH];

	init_lepton_info(&info, LEPTON_VERSION_3X, TELEMETRY_OFF);
	CHECK(info.next_subframe_index == 1);
	make_subframe(subframe, 1);
	CHECK(is_subframe_index_valid(&info, (unsigned short *)subframe));
	make_subframe(subframe, 2);
	CHECK(!is_subframe_index_valid(&info, (unsigned short *)subframe));
	make_subframe(subframe, 0);
	CHECK(is_subframe_index_valid(&info, (unsigned short *)subframe));
	CHECK(info.next_subframe_index == 1);

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	puts("lepton_vospi_funcs tests passed");
	return 0;
}
