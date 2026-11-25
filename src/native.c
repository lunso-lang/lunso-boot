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
        if (!fn.module && fn.name.length == 4 && memcmp(module->source + fn.name.offset, "main", 4) == 0) {
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

static bool check_link_inputs(const NativeOptions *options)
{
    for (size_t i = 0; i < options->object_count; i++) {
        const char *path = options->objects[i];
        struct stat info;
        if (stat(path, &info) != 0) {
            fprintf(stderr, "%s: could not read link input: %s\n", path, strerror(errno));
            return false;
        }
        if (!S_ISREG(info.st_mode)) {
            fprintf(stderr, "%s: link input must be a regular file\n", path);
            return false;
        }
        if (same_file(path, options->output)) {
            fprintf(stderr, "%s: output would overwrite a link input\n", options->output);
            return false;
        }
    }
    return true;
}

static bool link_program(const NativeOptions *options, char *assembly, char *program)
{
    size_t count = options->object_count;
    if (count > (SIZE_MAX / sizeof(char *) - 5) / 2) {
        fputs("too many link inputs\n", stderr);
        return false;
    }
    char **args = calloc(5 + count * 2, sizeof(*args));
    if (!args) {
        fputs("could not allocate linker arguments\n", stderr);
        return false;
    }
    char *cc = getenv("CC");
    args[0] = cc && *cc ? cc : "cc";
    args[1] = assembly;
    size_t i = 0;
    for (; i < count; i++) {
        const char *path = options->objects[i];
        args[2 + i * 2] = "-Xlinker";
        // Give the linker a file operand even if the relative path starts with '-' or '@'.
        args[3 + i * 2] = path[0] == '/' ? strdup(path) : join_path("./", path);
        if (!args[3 + i * 2]) {
            fputs("could not allocate a link input path\n", stderr);
            break;
        }
    }
    args[2 + count * 2] = "-o";
    args[3 + count * 2] = program;
    bool ok = i == count && run_tool(args);
    for (size_t j = 0; j < i; j++) {
        free(args[3 + j * 2]);
    }
    free(args);
    return ok;
}

bool native_build(const IrModule *module, const char *source, const NativeOptions *options)
{
    const char *output = options->output;
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
    if (!check_link_inputs(options)) {
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
        char *qbe_args[] = {qbe && *qbe ? qbe : "qbe", "-o", assembly, il, NULL};
        ok = write_il(module, il) && run_tool(qbe_args) &&
             link_program(options, assembly, program) && publish(program, output);
    }

    remove_temporary(il);
    remove_temporary(assembly);
    remove_temporary(program);
    rmdir(directory);
    free(directory);
    return ok;
}
