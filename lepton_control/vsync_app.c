#include <stdio.h>
#include <stdlib.h>

#include "FLIR_I2C.h"

#include "LEPTON_OEM.h"
#include "LEPTON_SDK.h"
#include "LEPTON_SYS.h"
#include "LEPTON_Types.h"

LEP_CAMERA_PORT_DESC_T lepton_port;

static LEP_RESULT init_vsync(void)
{
    LEP_RESULT result;
    LEP_OEM_GPIO_MODE_E gpio_mode;

    gpio_mode = LEP_OEM_END_GPIO_MODE;
    result = LEP_GetOemGpioMode(&lepton_port, &gpio_mode);
    printf("LEP_GetOemGpioMode gpio_mode = %d result = %d.\n", gpio_mode, result);
    if (result != LEP_OK)
        return result;

    result = LEP_SetOemGpioMode(&lepton_port, LEP_OEM_GPIO_MODE_VSYNC);
    printf("LEP_SetOemGpioMode result = %d.\n", result);
    if (result != LEP_OK)
        return result;

    gpio_mode = LEP_OEM_END_GPIO_MODE;
    result = LEP_GetOemGpioMode(&lepton_port, &gpio_mode);
    printf("LEP_GetOemGpioMode gpio_mode = %d result = %d.\n", gpio_mode, result);
    return result;
}

int main(int argc, char **argv)
{
    LEP_RESULT result;

    (void)argc;
    (void)argv;
    result = LEP_OpenPort(1, LEP_CCI_TWI, 400, &lepton_port);
    if (result != LEP_OK) {
        fprintf(stderr, "LEP_OpenPort failed: %d\n", result);
        return EXIT_FAILURE;
    }
    result = init_vsync();
    if (LEP_ClosePort(&lepton_port) != LEP_OK && result == LEP_OK)
        result = LEP_ERROR;
    return result == LEP_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}

