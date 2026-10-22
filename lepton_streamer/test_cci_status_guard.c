#define _POSIX_C_SOURCE 200809L
#include "cci_status_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); failures++; } } while (0)

struct probe { int calls, status, error; };
static int read_status(void *arg, int *status)
{
    struct probe *p = arg;
    p->calls++;
    *status = p->status;
    return p->error;
}

int main(void)
{
    char path[] = "/tmp/lepton-cci-guard-XXXXXX";
    struct probe p = {0, 1, 0};
    enum cci_guard_reason reason;
    int fd = mkstemp(path), err = 99, ready_pipe[2], release_pipe[2];
    pid_t child;
    char signal_byte;

    CHECK(fd >= 0);
    if (fd < 0) return 1;
    CHECK(cci_status_guard_check(fd, read_status, &p, 1, &err, &reason) == 1);
    CHECK(reason == CCI_GUARD_READY && err == 0 && p.calls == 1);
    p.status = 2;
    CHECK(cci_status_guard_check(fd, read_status, &p, 1, &err, &reason) == 0);
    CHECK(reason == CCI_GUARD_NOT_READY);
    p.error = -178;
    CHECK(cci_status_guard_check(fd, read_status, &p, 1, &err, &reason) == 0);
    CHECK(reason == CCI_GUARD_READ_FAILED && err == -178);
    p.error = 0;
    p.status = 1;
    CHECK(cci_status_guard_check(-1, read_status, &p, 1, &err, &reason) == 1);
    CHECK(cci_status_guard_check(-2, read_status, &p, 1, &err, &reason) == 1);

    CHECK(pipe(ready_pipe) == 0);
    CHECK(pipe(release_pipe) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        int other = open(path, O_RDONLY);
        close(ready_pipe[0]);
        close(release_pipe[1]);
        if (other < 0 || flock(other, LOCK_EX) != 0)
            _exit(2);
        if (write(ready_pipe[1], "1", 1) != 1)
            _exit(3);
        if (read(release_pipe[0], &signal_byte, 1) != 1)
            _exit(4);
        close(other);
        _exit(0);
    }
    if (child > 0) {
        int before, wstatus = 0;
        close(ready_pipe[1]);
        close(release_pipe[0]);
        CHECK(read(ready_pipe[0], &signal_byte, 1) == 1);
        before = p.calls;
        CHECK(cci_status_guard_check(fd, read_status, &p, 1, &err, &reason) == 0);
        CHECK(reason == CCI_GUARD_BUSY && p.calls == before);
        CHECK(write(release_pipe[1], "1", 1) == 1);
        CHECK(waitpid(child, &wstatus, 0) == child);
        CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
        CHECK(cci_status_guard_check(fd, read_status, &p, 1, &err, &reason) == 1);
        CHECK(p.calls == before + 1);
        close(ready_pipe[0]);
        close(release_pipe[1]);
    }
    close(fd);
    unlink(path);
    if (failures) fprintf(stderr, "%d CCI guard tests failed\n", failures);
    return failures != 0;
}
