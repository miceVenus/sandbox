#define _GNU_SOURCE
#include "virtualization/container/crun_ops.h"
#include <errno.h>
#include <libcrun/container.h>
#include <libcrun/custom-handler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// libcrun calls run in a disposable process: cwd, signals and descriptors
// never belong to the embedding SDK service. No command-line parsing here.
static int report_error(libcrun_error_t *error) {
    if (*error) {
        fprintf(stderr, "libcrun: %s\n", (*error)->msg);
        libcrun_error_release(error);
    }
    return 125;
}

static int start_container(libcrun_context_t *context, const char *bundle, libcrun_error_t *error) {
    context->bundle = bundle;
    context->detach = true;
    if (bundle[0] != '/' || chdir(bundle) < 0) {
        libcrun_make_error(error, errno, "cannot enter bundle directory");
        return -1;
    }
    libcrun_container_t *container = libcrun_container_load_from_file("config.json", error);
    if (!container) {
        return -1;
    }
    int result = libcrun_container_run(context, container, 0, error);
    libcrun_container_free(container);
    return result;
}

int sandbox_libcrun_dispatch(const struct sandbox_crun_request *request, int argc, char **argv) {
    libcrun_context_t context = {0};
    libcrun_error_t error = NULL;
    context.fifo_exec_wait_fd = -1;
    context.argc = argc;
    context.argv = argv;
    context.state_root = request->state_root;
    context.id = request->id;
    context.systemd_cgroup = request->systemd_cgroups;
    context.preserve_fds = request->preserve_fds;
    if (libcrun_close_inherited_fds(&context, &error) < 0 ||
        libcrun_init_logging(&context.output_handler, &context.output_handler_arg, context.id, NULL,
                             &error) < 0) {
        return report_error(&error);
    }
    context.handler_manager = libcrun_handler_manager_create(&error);
    if (!context.handler_manager) {
        return report_error(&error);
    }
    int result = -1;
    switch (request->operation) {
    case SANDBOX_CRUN_START:
        result = start_container(&context, request->bundle, &error);
        break;
    case SANDBOX_CRUN_STATE:
        result = libcrun_container_state(&context, context.id, stdout, &error);
        break;
    case SANDBOX_CRUN_PAUSE:
        result = libcrun_container_pause(&context, context.id, &error);
        break;
    case SANDBOX_CRUN_RESUME:
        result = libcrun_container_unpause(&context, context.id, &error);
        break;
    case SANDBOX_CRUN_KILL:
        result = libcrun_container_killall(&context, context.id, "KILL", &error);
        break;
    case SANDBOX_CRUN_DELETE:
        result = libcrun_container_delete(&context, NULL, context.id, true, &error);
        break;
    }
    handler_manager_free(context.handler_manager);
    if (result < 0) {
        return report_error(&error);
    }
    return result;
}
