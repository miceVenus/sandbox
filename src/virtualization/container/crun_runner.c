#define _GNU_SOURCE
#include <errno.h>
#include <libcrun/container.h>
#include <libcrun/custom-handler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// SDK-owned, single-operation worker. stdin/stdout/stderr belong to the task;
// only fixed runtime operations are accepted, without arbitrary runtime flags.
static int usage(void) {
    fprintf(stderr, "invalid SDK runtime request\n");
    return 125;
}

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

static int execute_process(libcrun_context_t *context,
                           const char *cwd,
                           int count,
                           char **arguments,
                           libcrun_error_t *error) {
    runtime_spec_schema_config_schema_process *process = calloc(1, sizeof(*process));
    if (!process) {
        return libcrun_make_error(error, ENOMEM, "allocate task process");
    }
    process->args_len = (size_t)count;
    process->args = calloc((size_t)count + 1, sizeof(char *));
    process->cwd = strdup(cwd);
    process->no_new_privileges = true;
    process->no_new_privileges_present = true;
    if (!process->args || !process->cwd) {
        free_runtime_spec_schema_config_schema_process(process);
        return libcrun_make_error(error, ENOMEM, "allocate task arguments");
    }
    for (int i = 0; i < count; ++i) {
        process->args[i] = strdup(arguments[i]);
        if (!process->args[i]) {
            free_runtime_spec_schema_config_schema_process(process);
            return libcrun_make_error(error, ENOMEM, "allocate task argument");
        }
    }
    struct libcrun_container_exec_options_s options = {0};
    options.struct_size = sizeof(options);
    options.process = process;
    options.merge_env = true;
    int result = libcrun_container_exec_with_options(context, context->id, &options, error);
    free_runtime_spec_schema_config_schema_process(process);
    return result;
}

int main(int argc, char **argv) {
    libcrun_context_t context = {0};
    libcrun_error_t error = NULL;
    context.fifo_exec_wait_fd = -1;
    context.argc = argc;
    context.argv = argv;
    int next = 1;
    if (next < argc && strcmp(argv[next], "--systemd-cgroup") == 0) {
        context.systemd_cgroup = true;
        ++next;
    }
    if (next + 2 >= argc || strcmp(argv[next], "--root") != 0) {
        return usage();
    }
    context.state_root = argv[next + 1];
    if (context.state_root[0] != '/') {
        return usage();
    }
    next += 2;
    const char *command = argv[next++];
    const char *bundle = NULL;
    const char *cwd = NULL;
    if (strcmp(command, "run") == 0) {
        if (next + 3 >= argc || strcmp(argv[next], "--detach") != 0 ||
            strcmp(argv[next + 1], "--bundle") != 0) {
            return usage();
        }
        bundle = argv[next + 2];
        next += 3;
    } else if (strcmp(command, "exec") == 0) {
        if (next + 4 >= argc || strcmp(argv[next], "--no-new-privs") != 0 ||
            strcmp(argv[next + 1], "--cwd") != 0) {
            return usage();
        }
        cwd = argv[next + 2];
        next += 3;
    } else if (strcmp(command, "kill") == 0) {
        if (next >= argc || strcmp(argv[next++], "--all") != 0) {
            return usage();
        }
    } else if (strcmp(command, "delete") == 0) {
        if (next >= argc || strcmp(argv[next++], "--force") != 0) {
            return usage();
        }
    } else if (strcmp(command, "state") != 0 && strcmp(command, "pause") != 0 &&
               strcmp(command, "resume") != 0) {
        return usage();
    }
    if (next >= argc) {
        return usage();
    }
    context.id = argv[next++];
    if (context.id[0] == '\0' || strchr(context.id, '/') || context.id[0] == '.') {
        return usage();
    }
    if (strcmp(command, "exec") != 0 && strcmp(command, "kill") != 0 && next != argc) {
        return usage();
    }
    if (strcmp(command, "kill") == 0 && (next + 1 != argc || strcmp(argv[next], "KILL") != 0)) {
        return usage();
    }
    if (libcrun_close_inherited_fds(&context, &error) < 0 ||
        libcrun_init_logging(
            &context.output_handler, &context.output_handler_arg, context.id, NULL, &error) < 0) {
        return report_error(&error);
    }
    context.handler_manager = libcrun_handler_manager_create(&error);
    if (!context.handler_manager) {
        return report_error(&error);
    }
    int result = -1;
    if (strcmp(command, "run") == 0) {
        result = start_container(&context, bundle, &error);
    } else if (strcmp(command, "exec") == 0) {
        result = execute_process(&context, cwd, argc - next, argv + next, &error);
    } else if (strcmp(command, "state") == 0) {
        result = libcrun_container_state(&context, context.id, stdout, &error);
    } else if (strcmp(command, "pause") == 0) {
        result = libcrun_container_pause(&context, context.id, &error);
    } else if (strcmp(command, "resume") == 0) {
        result = libcrun_container_unpause(&context, context.id, &error);
    } else if (strcmp(command, "kill") == 0) {
        result = libcrun_container_killall(&context, context.id, "KILL", &error);
    } else if (strcmp(command, "delete") == 0) {
        result = libcrun_container_delete(&context, NULL, context.id, true, &error);
    }
    handler_manager_free(context.handler_manager);
    if (result < 0) {
        return report_error(&error);
    }
    return result;
}
