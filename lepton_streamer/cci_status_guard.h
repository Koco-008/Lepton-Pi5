#ifndef LEPTON_CCI_STATUS_GUARD_H
#define LEPTON_CCI_STATUS_GUARD_H

enum cci_guard_reason {
    CCI_GUARD_READY = 0,
    CCI_GUARD_BUSY,
    CCI_GUARD_NOT_READY,
    CCI_GUARD_READ_FAILED,
    CCI_GUARD_LOCK_FAILED
};

typedef int (*cci_status_reader)(void *context, int *status);

/* Return 1 only for a verified READY status, 0 for a dropped frame, -1 for
 * a fatal lock failure. lock_fd == -1 preserves standalone CLI compatibility.
 * The status reader executes entirely under the same flock as SPOT's CCI
 * operations; a busy lock never permits an unverified frame. */
int cci_status_guard_check(int lock_fd, cci_status_reader reader, void *context,
                           int ready_status, int *device_error,
                           enum cci_guard_reason *reason);

#endif
