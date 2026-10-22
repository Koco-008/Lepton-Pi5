#define _POSIX_C_SOURCE 200809L
#include "cci_status_guard.h"

#include <errno.h>
#include <sys/file.h>

int cci_status_guard_check(int lock_fd, cci_status_reader reader, void *context,
                           int ready_status, int *device_error,
                           enum cci_guard_reason *reason)
{
    int status = 0;
    int result;

    if (device_error != NULL)
        *device_error = 0;
    if (reason != NULL)
        *reason = CCI_GUARD_LOCK_FAILED;
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            if (reason != NULL)
                *reason = CCI_GUARD_BUSY;
            return 0;
        }
        return -1;
    }
    result = reader(context, &status);
    /* Release before processing the result: no formatting, rendering,
     * sleeps, V4L2 work or recovery is performed under the CCI lock. */
    if (lock_fd >= 0 && flock(lock_fd, LOCK_UN) != 0)
        return -1;
    if (result != 0) {
        if (device_error != NULL)
            *device_error = result;
        if (reason != NULL)
            *reason = CCI_GUARD_READ_FAILED;
        return 0;
    }
    if (status != ready_status) {
        if (reason != NULL)
            *reason = CCI_GUARD_NOT_READY;
        return 0;
    }
    if (reason != NULL)
        *reason = CCI_GUARD_READY;
    return 1;
}
