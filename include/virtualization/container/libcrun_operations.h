#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Private SDK operations, not crun command-line arguments.
enum sandbox_crun_operation {
    SANDBOX_CRUN_START,
    SANDBOX_CRUN_STATE,
    SANDBOX_CRUN_PAUSE,
    SANDBOX_CRUN_RESUME,
    SANDBOX_CRUN_KILL,
    SANDBOX_CRUN_DELETE
};

struct sandbox_crun_request {
    enum sandbox_crun_operation operation;
    const char *state_root;
    const char *id;
    const char *bundle;
    bool systemd_cgroups;
    int preserve_fds;
};

// Called only in the isolated worker, after consuming and closing its control FD.
int sandbox_libcrun_dispatch(const struct sandbox_crun_request *request, int argc, char **argv);

#ifdef __cplusplus
}
#endif
