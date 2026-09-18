#include "radiometry_config.h"

static FILE *stream_or_stderr(FILE *stream)
{
	return stream ? stream : stderr;
}

static int report_sdk_error(FILE *stream, const char *operation, LEP_RESULT result)
{
	fprintf(stream_or_stderr(stream), "%s failed: %d\n", operation, result);
	return -1;
}

int lepton_read_radiometry_status(
	LEP_CAMERA_PORT_DESC_T *port,
	struct lepton_radiometry_status *status,
	FILE *error_stream)
{
	LEP_RESULT result;

	if (!port || !status) {
		fprintf(stream_or_stderr(error_stream),
			"Cannot read Lepton radiometry status: invalid argument\n");
		return -1;
	}

	result = LEP_GetRadEnableState(port, &status->radiometry);
	if (result != LEP_OK)
		return report_sdk_error(error_stream, "LEP_GetRadEnableState", result);
	result = LEP_GetRadTLinearEnableState(port, &status->tlinear);
	if (result != LEP_OK)
		return report_sdk_error(error_stream,
			"LEP_GetRadTLinearEnableState", result);
	result = LEP_GetRadTLinearAutoResolution(port, &status->auto_resolution);
	if (result != LEP_OK)
		return report_sdk_error(error_stream,
			"LEP_GetRadTLinearAutoResolution", result);
	result = LEP_GetRadTLinearResolution(port, &status->resolution);
	if (result != LEP_OK)
		return report_sdk_error(error_stream,
			"LEP_GetRadTLinearResolution", result);

	return 0;
}

int lepton_radiometry_is_kelvin_x100(
	const struct lepton_radiometry_status *status)
{
	return status &&
		status->radiometry == LEP_RAD_ENABLE &&
		status->tlinear == LEP_RAD_ENABLE &&
		status->auto_resolution == LEP_RAD_DISABLE &&
		status->resolution == LEP_RAD_RESOLUTION_0_01;
}

void lepton_print_radiometry_status(
	const struct lepton_radiometry_status *status,
	FILE *stream)
{
	FILE *output = stream ? stream : stdout;
	unsigned int scale = 0;
	const char *resolution = "unknown";

	if (!status)
		return;
	if (status->resolution == LEP_RAD_RESOLUTION_0_1) {
		resolution = "0.1";
		scale = 10;
	} else if (status->resolution == LEP_RAD_RESOLUTION_0_01) {
		resolution = "0.01";
		scale = 100;
	}

	fprintf(output, "radiometry_enabled=%d\n", status->radiometry);
	fprintf(output, "tlinear_enabled=%d\n", status->tlinear);
	fprintf(output, "tlinear_auto_resolution=%d\n", status->auto_resolution);
	fprintf(output, "tlinear_resolution_kelvin=%s\n", resolution);
	fprintf(output, "tlinear_scale=%u\n", scale);
}

int lepton_configure_radiometry_kelvin_x100(
	LEP_CAMERA_PORT_DESC_T *port,
	FILE *log_stream)
{
	struct lepton_radiometry_status before;
	struct lepton_radiometry_status after;
	FILE *log = stream_or_stderr(log_stream);
	LEP_RESULT result;

	if (lepton_read_radiometry_status(port, &before, log) != 0)
		return -1;

	fprintf(log,
		"Lepton radiometry before configure: radiometry=%d tlinear=%d "
		"auto_resolution=%d resolution=%d\n",
		before.radiometry, before.tlinear, before.auto_resolution,
		before.resolution);

	if (before.radiometry != LEP_RAD_ENABLE) {
		result = LEP_SetRadEnableState(port, LEP_RAD_ENABLE);
		if (result != LEP_OK)
			return report_sdk_error(log, "LEP_SetRadEnableState(ENABLE)", result);
	}
	if (before.auto_resolution != LEP_RAD_DISABLE) {
		result = LEP_SetRadTLinearAutoResolution(port, LEP_RAD_DISABLE);
		if (result != LEP_OK)
			return report_sdk_error(log,
				"LEP_SetRadTLinearAutoResolution(DISABLE)", result);
	}
	if (before.resolution != LEP_RAD_RESOLUTION_0_01) {
		result = LEP_SetRadTLinearResolution(port, LEP_RAD_RESOLUTION_0_01);
		if (result != LEP_OK)
			return report_sdk_error(log,
				"LEP_SetRadTLinearResolution(0.01K)", result);
	}
	if (before.tlinear != LEP_RAD_ENABLE) {
		result = LEP_SetRadTLinearEnableState(port, LEP_RAD_ENABLE);
		if (result != LEP_OK)
			return report_sdk_error(log,
				"LEP_SetRadTLinearEnableState(ENABLE)", result);
	}

	if (lepton_read_radiometry_status(port, &after, log) != 0)
		return -1;
	if (!lepton_radiometry_is_kelvin_x100(&after)) {
		fprintf(log,
			"Lepton radiometry verification failed: radiometry=%d "
			"tlinear=%d auto_resolution=%d resolution=%d\n",
			after.radiometry, after.tlinear, after.auto_resolution,
			after.resolution);
		return -1;
	}

	fprintf(log,
		"Lepton radiometry configured and verified: radiometry=1 "
		"tlinear=1 auto_resolution=0 resolution=0.01K scale=100\n");
	return 0;
}
