#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "radiometry_config.h"

enum fake_call {
	FAKE_NONE,
	FAKE_GET_RAD,
	FAKE_GET_TLINEAR,
	FAKE_GET_AUTO,
	FAKE_GET_RESOLUTION,
	FAKE_SET_RAD,
	FAKE_SET_TLINEAR,
	FAKE_SET_AUTO,
	FAKE_SET_RESOLUTION,
};

static struct lepton_radiometry_status fake_status;
static enum fake_call failing_call;
static int ignore_tlinear_set;
static char calls[64];
static size_t call_count;

static void record_call(char call)
{
	assert(call_count + 1 < sizeof(calls));
	calls[call_count++] = call;
	calls[call_count] = '\0';
}

static void reset_fake(void)
{
	fake_status = (struct lepton_radiometry_status) {
		.radiometry = LEP_RAD_DISABLE,
		.tlinear = LEP_RAD_DISABLE,
		.auto_resolution = LEP_RAD_ENABLE,
		.resolution = LEP_RAD_RESOLUTION_0_1,
	};
	failing_call = FAKE_NONE;
	ignore_tlinear_set = 0;
	call_count = 0;
	calls[0] = '\0';
}

static LEP_RESULT fake_result(enum fake_call call)
{
	return failing_call == call ? LEP_ERROR : LEP_OK;
}

LEP_RESULT LEP_GetRadEnableState(
	LEP_CAMERA_PORT_DESC_T_PTR port, LEP_RAD_ENABLE_E_PTR value)
{
	(void)port;
	record_call('R');
	if (fake_result(FAKE_GET_RAD) != LEP_OK)
		return LEP_ERROR;
	*value = fake_status.radiometry;
	return LEP_OK;
}

LEP_RESULT LEP_GetRadTLinearEnableState(
	LEP_CAMERA_PORT_DESC_T_PTR port, LEP_RAD_ENABLE_E_PTR value)
{
	(void)port;
	record_call('T');
	if (fake_result(FAKE_GET_TLINEAR) != LEP_OK)
		return LEP_ERROR;
	*value = fake_status.tlinear;
	return LEP_OK;
}

LEP_RESULT LEP_GetRadTLinearAutoResolution(
	LEP_CAMERA_PORT_DESC_T_PTR port, LEP_RAD_ENABLE_E_PTR value)
{
	(void)port;
	record_call('A');
	if (fake_result(FAKE_GET_AUTO) != LEP_OK)
		return LEP_ERROR;
	*value = fake_status.auto_resolution;
	return LEP_OK;
}

LEP_RESULT LEP_GetRadTLinearResolution(
	LEP_CAMERA_PORT_DESC_T_PTR port, LEP_RAD_TLINEAR_RESOLUTION_E_PTR value)
{
	(void)port;
	record_call('S');
	if (fake_result(FAKE_GET_RESOLUTION) != LEP_OK)
		return LEP_ERROR;
	*value = fake_status.resolution;
	return LEP_OK;
}

LEP_RESULT LEP_SetRadEnableState(
	LEP_CAMERA_PORT_DESC_T_PTR port, LEP_RAD_ENABLE_E value)
{
	(void)port;
	record_call('r');
	if (fake_result(FAKE_SET_RAD) != LEP_OK)
		return LEP_ERROR;
	fake_status.radiometry = value;
	return LEP_OK;
}

LEP_RESULT LEP_SetRadTLinearEnableState(
	LEP_CAMERA_PORT_DESC_T_PTR port, LEP_RAD_ENABLE_E value)
{
	(void)port;
	record_call('t');
	if (fake_result(FAKE_SET_TLINEAR) != LEP_OK)
		return LEP_ERROR;
	if (!ignore_tlinear_set)
		fake_status.tlinear = value;
	return LEP_OK;
}

LEP_RESULT LEP_SetRadTLinearAutoResolution(
	LEP_CAMERA_PORT_DESC_T_PTR port, LEP_RAD_ENABLE_E value)
{
	(void)port;
	record_call('a');
	if (fake_result(FAKE_SET_AUTO) != LEP_OK)
		return LEP_ERROR;
	fake_status.auto_resolution = value;
	return LEP_OK;
}

LEP_RESULT LEP_SetRadTLinearResolution(
	LEP_CAMERA_PORT_DESC_T_PTR port, LEP_RAD_TLINEAR_RESOLUTION_E value)
{
	(void)port;
	record_call('s');
	if (fake_result(FAKE_SET_RESOLUTION) != LEP_OK)
		return LEP_ERROR;
	fake_status.resolution = value;
	return LEP_OK;
}

static FILE *open_log(void)
{
	FILE *log = tmpfile();

	assert(log != NULL);
	return log;
}

static void test_configures_and_verifies_all_fields(void)
{
	LEP_CAMERA_PORT_DESC_T port = { 0 };
	FILE *log = open_log();

	reset_fake();
	assert(lepton_configure_radiometry_kelvin_x100(&port, log) == 0);
	assert(strcmp(calls, "RTASrastRTAS") == 0);
	assert(lepton_radiometry_is_kelvin_x100(&fake_status));
	fclose(log);
}

static void test_already_configured_is_idempotent(void)
{
	LEP_CAMERA_PORT_DESC_T port = { 0 };
	FILE *log = open_log();

	reset_fake();
	fake_status.radiometry = LEP_RAD_ENABLE;
	fake_status.tlinear = LEP_RAD_ENABLE;
	fake_status.auto_resolution = LEP_RAD_DISABLE;
	fake_status.resolution = LEP_RAD_RESOLUTION_0_01;
	assert(lepton_configure_radiometry_kelvin_x100(&port, log) == 0);
	assert(strcmp(calls, "RTASRTAS") == 0);
	fclose(log);
}

static void test_sdk_failure_is_fatal(void)
{
	LEP_CAMERA_PORT_DESC_T port = { 0 };
	FILE *log = open_log();

	reset_fake();
	failing_call = FAKE_SET_RESOLUTION;
	assert(lepton_configure_radiometry_kelvin_x100(&port, log) != 0);
	assert(strcmp(calls, "RTASras") == 0);
	fclose(log);
}

static void test_verification_mismatch_is_fatal(void)
{
	LEP_CAMERA_PORT_DESC_T port = { 0 };
	FILE *log = open_log();

	reset_fake();
	ignore_tlinear_set = 1;
	assert(lepton_configure_radiometry_kelvin_x100(&port, log) != 0);
	assert(strcmp(calls, "RTASrastRTAS") == 0);
	fclose(log);
}

int main(void)
{
	test_configures_and_verifies_all_fields();
	test_already_configured_is_idempotent();
	test_sdk_failure_is_fatal();
	test_verification_mismatch_is_fatal();
	puts("radiometry configuration tests passed");
	return 0;
}
