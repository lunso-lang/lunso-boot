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
        if (!fn.template_only && !fn.specialization && !fn.module && fn.name.length == 4 &&
            memcmp(module->source + fn.name.offset, "main", 4) == 0) {
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
    size_t extra = options->nostdlib ? 1 : 0;
    if (count > (SIZE_MAX / sizeof(char *) - 6 - extra) / 2) {
        fputs("too many link inputs\n", stderr);
        return false;
    }
    char **args = calloc(5 + extra + count * 2, sizeof(*args));
    if (!args) {
        fputs("could not allocate linker arguments\n", stderr);
        return false;
    }
    char *cc = getenv("CC");
    size_t idx = 0;
    args[idx++] = cc && *cc ? cc : "cc";
    if (options->nostdlib) {
        args[idx++] = "-nostdlib";
    }
    args[idx++] = assembly;
    size_t i = 0;
    for (; i < count; i++) {
        const char *path = options->objects[i];
        args[idx++] = "-Xlinker";
        // Give the linker a file operand even if the relative path starts with '-' or '@'.
        char *operand = path[0] == '/' ? strdup(path) : join_path("./", path);
        args[idx++] = operand;
        if (!operand) {
            fputs("could not allocate a link input path\n", stderr);
            break;
        }
    }
    args[idx++] = "-o";
    args[idx++] = program;
    bool ok = i == count && run_tool(args);
    size_t base = 2 + extra;
    for (size_t j = 0; j < i; j++) {
        free(args[base + j * 2 + 1]);
    }
    free(args);
    return ok;
}

static bool assemble_object(char *assembly, char *object)
{
    char *cc = getenv("CC");
    char *args[] = {cc && *cc ? cc : "cc", "-c", assembly, "-o", object, NULL};
    return run_tool(args);
}

bool native_build(const IrModule *module, const char *source, const NativeOptions *options)
{
    const char *output = options->output;
    if (options->kind == NATIVE_EXE && !has_entry_point(module)) {
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
    if (options->kind == NATIVE_EXE && !check_link_inputs(options)) {
        return false;
    }

    char *directory = make_directory(output);
    if (!directory) {
        perror(output);
        return false;
    }
    char *il = join_path(directory, "/module.ssa");
    char *assembly = join_path(directory, "/module.s");
    char *out_tmp = options->kind == NATIVE_ASM ? NULL
                                                : join_path(directory, options->kind == NATIVE_OBJ ? "/module.o" : "/program");
    bool ok = false;
    if (!il || !assembly || (options->kind != NATIVE_ASM && !out_tmp)) {
        fprintf(stderr, "%s: could not allocate output paths\n", output);
    } else {
        char *qbe = getenv("QBE");
        char *qbe_args[] = {qbe && *qbe ? qbe : "qbe", "-o", assembly, il, NULL};
        if (write_il(module, il) && run_tool(qbe_args)) {
            if (options->kind == NATIVE_ASM) {
                ok = publish(assembly, output);
            } else if (options->kind == NATIVE_OBJ) {
                ok = assemble_object(assembly, out_tmp) && publish(out_tmp, output);
            } else {
                ok = link_program(options, assembly, out_tmp) && publish(out_tmp, output);
            }
        }
    }

    remove_temporary(il);
    remove_temporary(assembly);
    remove_temporary(out_tmp);
    rmdir(directory);
    free(directory);
    return ok;
}

int native_run(const char *program, int argc, char *const *argv)
{
    size_t extra = argc > 0 ? (size_t)argc : 0;
    char **args = calloc(extra + 2, sizeof(*args));
    if (!args) {
        fputs("could not allocate run arguments\n", stderr);
        return EXIT_FAILURE;
    }
    args[0] = (char *)program;
    for (size_t i = 0; i < extra; i++) {
        args[i + 1] = argv[i];
    }
    pid_t child;
    int spawn_error = posix_spawnp(&child, program, NULL, NULL, args, environ);
    free(args);
    if (spawn_error) {
        fprintf(stderr, "%s: could not run: %s\n", program, strerror(spawn_error));
        return EXIT_FAILURE;
    }

    int status;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        perror(program);
        return EXIT_FAILURE;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return EXIT_FAILURE;
}
