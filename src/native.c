#define _POSIX_C_SOURCE 200809L

#include "native.h"
#include "qbe.h"

#include <errno.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static bool has_entry_point(const IrModule *module)
{
    for (uint32_t i = 0; i < module->function_count; i++) {
        IrFunction fn = module->functions[i];
        if (fn.name.length == 4 && memcmp(module->source + fn.name.offset, "main", 4) == 0) {
            return !fn.external && fn.result.kind == TYPE_I32 && fn.parameter_count == 0;
        }
    }
    return false;
}

static bool same_file(const char *source, const char *output)
{
    struct stat input_info, output_info;
    return stat(source, &input_info) == 0 && stat(output, &output_info) == 0 &&
           input_info.st_dev == output_info.st_dev && input_info.st_ino == output_info.st_ino;
}

static char *make_directory(const char *output)
{
    const char *slash = strrchr(output, '/');
    size_t parent = slash ? (size_t)(slash - output) + 1 : 0;
    const char *prefix = output[0] == '/' ? "" : "./";
    size_t prefix_length = strlen(prefix);
    static const char pattern[] = ".lunso-XXXXXX";
    if (parent > SIZE_MAX - prefix_length - sizeof(pattern)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    char *path = malloc(prefix_length + parent + sizeof(pattern));
    if (!path) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(path, prefix, prefix_length);
    memcpy(path + prefix_length, output, parent);
    memcpy(path + prefix_length + parent, pattern, sizeof(pattern));
    if (!mkdtemp(path)) {
        int error = errno;
        free(path);
        errno = error;
        return NULL;
    }
    return path;
}

static char *join_path(const char *directory, const char *suffix)
{
    size_t prefix = strlen(directory);
    size_t tail = strlen(suffix) + 1;
    if (prefix > SIZE_MAX - tail) {
        return NULL;
    }
    char *path = malloc(prefix + tail);
    if (path) {
        memcpy(path, directory, prefix);
        memcpy(path + prefix, suffix, tail);
    }
    return path;
}

static bool write_il(const IrModule *module, const char *file)
{
    FILE *output = fopen(file, "w");
    if (!output) {
        perror(file);
        return false;
    }
    bool written = qbe_write(output, module);
    if (fclose(output) != 0) {
        perror(file);
        return false;
    }
    if (!written) {
        fprintf(stderr, "%s: could not write QBE IL\n", file);
    }
    return written;
}

static bool run_tool(char *const args[])
{
    pid_t child;
    int spawn_error = posix_spawnp(&child, args[0], NULL, NULL, args, environ);
    if (spawn_error) {
        fprintf(stderr, "%s: could not start: %s\n", args[0], strerror(spawn_error));
        return false;
    }

    int status;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        perror(args[0]);
        return false;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return true;
    }
    if (WIFEXITED(status)) {
        fprintf(stderr, "%s: exited with status %d\n", args[0], WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s: terminated by signal %d\n", args[0], WTERMSIG(status));
    }
    return false;
}

static bool publish(const char *temporary, const char *output)
{
    if (rename(temporary, output) != 0) {
        perror(output);
        return false;
    }
    return true;
}

static void remove_temporary(char *path)
{
    if (path) {
        unlink(path);
        free(path);
    }
}

bool native_build(const IrModule *module, const char *source, const char *output)
{
    if (!has_entry_point(module)) {
        fprintf(stderr, "%s: expected entry point 'fun main(): i32'\n", source);
        return false;
    }
    if (!*output) {
        fprintf(stderr, "output path is empty\n");
        return false;
    }
    if (same_file(source, output)) {
        fprintf(stderr, "%s: output would overwrite the source file\n", output);
        return false;
    }

    char *directory = make_directory(output);
    if (!directory) {
        perror(output);
        return false;
    }
    char *il = join_path(directory, "/module.ssa");
    char *assembly = join_path(directory, "/module.s");
    char *program = join_path(directory, "/program");
    bool ok = false;
    if (!il || !assembly || !program) {
        fprintf(stderr, "%s: could not allocate output paths\n", output);
    } else {
        char *qbe = getenv("QBE");
        char *cc = getenv("CC");
        char *qbe_args[] = {qbe && *qbe ? qbe : "qbe", "-o", assembly, il, NULL};
        char *cc_args[] = {cc && *cc ? cc : "cc", assembly, "-o", program, NULL};
        ok = write_il(module, il) && run_tool(qbe_args) && run_tool(cc_args) && publish(program, output);
    }

    remove_temporary(il);
    remove_temporary(assembly);
    remove_temporary(program);
    rmdir(directory);
    free(directory);
    return ok;
}
