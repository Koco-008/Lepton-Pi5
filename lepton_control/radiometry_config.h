#ifndef LEPTON_RADIOMETRY_CONFIG_H
#define LEPTON_RADIOMETRY_CONFIG_H

#include <stdio.h>

#include "LEPTON_RAD.h"
#include "LEPTON_SDK.h"

struct lepton_radiometry_status {
	LEP_RAD_ENABLE_E radiometry;
	LEP_RAD_ENABLE_E tlinear;
	LEP_RAD_ENABLE_E auto_resolution;
	LEP_RAD_TLINEAR_RESOLUTION_E resolution;
};

int lepton_read_radiometry_status(
	LEP_CAMERA_PORT_DESC_T *port,
	struct lepton_radiometry_status *status,
	FILE *error_stream);
int lepton_configure_radiometry_kelvin_x100(
	LEP_CAMERA_PORT_DESC_T *port,
	FILE *log_stream);
int lepton_radiometry_is_kelvin_x100(
	const struct lepton_radiometry_status *status);
void lepton_print_radiometry_status(
	const struct lepton_radiometry_status *status,
	FILE *stream);

#endif
