#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NOB_VERSION
#define NOB_VERSION "development"
#endif

#ifndef NOB_INSTALLED_HEADER
#define NOB_INSTALLED_HEADER "nob.h"
#endif

#define NOB_CLI_PATH_CAPACITY 4096

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  nob init [--force] [directory]\n"
            "  nob run [recipe arguments...]\n"
            "  nob [recipe arguments...]\n"
            "  nob --version\n"
            "\n"
            "Commands:\n"
            "  init       Copy the installed nob.h into a project.\n"
            "  run        Build ./nob.c into ./.nob/nob when needed, then run it.\n"
            "\n"
            "Unknown arguments are passed directly to the project's build recipe.\n");
}

static bool path_join_header(char *buffer, size_t capacity, const char *directory)
{
    int length = snprintf(buffer, capacity, "%s%s%s", directory,
                          directory[0] != '\0' && directory[strlen(directory) - 1] == '/' ? "" : "/",
                          "nob.h");
    return length >= 0 && (size_t)length < capacity;
}

static int copy_installed_header(const char *directory, bool force)
{
    char target[NOB_CLI_PATH_CAPACITY];
    char temporary[NOB_CLI_PATH_CAPACITY];

    if (!path_join_header(target, sizeof(target), directory)) {
        fprintf(stderr, "nob: target path is too long\n");
        return 1;
    }

    if (!force && access(target, F_OK) == 0) {
        fprintf(stderr, "nob: %s already exists; use --force to replace it\n", target);
        return 1;
    }

    int length = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", target, (long)getpid());
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        fprintf(stderr, "nob: temporary path is too long\n");
        return 1;
    }

    FILE *source = fopen(NOB_INSTALLED_HEADER, "rb");
    if (source == NULL) {
        fprintf(stderr, "nob: could not open installed header %s: %s\n",
                NOB_INSTALLED_HEADER, strerror(errno));
        return 1;
    }

    FILE *destination = fopen(temporary, "wb");
    if (destination == NULL) {
        fprintf(stderr, "nob: could not create %s: %s\n", temporary, strerror(errno));
        fclose(source);
        return 1;
    }

    char buffer[64 * 1024];
    bool copied = true;
    while (!feof(source)) {
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), source);
        if (bytes_read > 0 && fwrite(buffer, 1, bytes_read, destination) != bytes_read) {
            copied = false;
            break;
        }
        if (ferror(source)) {
            copied = false;
            break;
        }
    }

    if (fclose(source) != 0) copied = false;
    if (fflush(destination) != 0) copied = false;
    if (fsync(fileno(destination)) != 0) copied = false;
    if (fclose(destination) != 0) copied = false;

    if (!copied) {
        fprintf(stderr, "nob: failed while copying %s to %s\n", NOB_INSTALLED_HEADER, target);
        unlink(temporary);
        return 1;
    }

    if (rename(temporary, target) != 0) {
        fprintf(stderr, "nob: could not install %s: %s\n", target, strerror(errno));
        unlink(temporary);
        return 1;
    }

    printf("Installed %s\n", target);
    return 0;
}

static bool is_directory(const char *path)
{
    struct stat metadata;
    return stat(path, &metadata) == 0 && S_ISDIR(metadata.st_mode);
}

static bool needs_rebuild(void)
{
    struct stat binary;
    struct stat source;
    struct stat header;

    if (stat(".nob/nob", &binary) != 0) return true;
    if (stat("nob.c", &source) != 0) return true;
    if (stat("nob.h", &header) != 0) return true;

    return source.st_mtime > binary.st_mtime || header.st_mtime > binary.st_mtime;
}

static int wait_for_child(pid_t child, const char *operation)
{
    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        fprintf(stderr, "nob: could not wait for %s: %s\n", operation, strerror(errno));
        return 1;
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "nob: %s was terminated by signal %d\n", operation, WTERMSIG(status));
    }
    return 1;
}

static int build_recipe(void)
{
    const char *compiler = getenv("CC");
    if (compiler == NULL || compiler[0] == '\0') compiler = "cc";

    if (!is_directory(".nob")) {
        if (mkdir(".nob", 0777) != 0 && (errno != EEXIST || !is_directory(".nob"))) {
            fprintf(stderr, "nob: could not create .nob: %s\n", strerror(errno));
            return 1;
        }
    }

    fprintf(stderr, "nob: building .nob/nob from nob.c\n");
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "nob: could not start compiler: %s\n", strerror(errno));
        return 1;
    }

    if (child == 0) {
        char *const compiler_arguments[] = {
            (char *)compiler,
            "-std=c11",
            "-o",
            ".nob/nob",
            "nob.c",
            NULL,
        };
        execvp(compiler, compiler_arguments);
        fprintf(stderr, "nob: could not run compiler %s: %s\n", compiler, strerror(errno));
        _exit(127);
    }

    return wait_for_child(child, "compiler");
}

static int run_recipe(int argument_count, char **arguments)
{
    if (access("nob.c", R_OK) != 0) {
        fprintf(stderr, "nob: no readable nob.c found in the current directory\n");
        return 1;
    }
    if (access("nob.h", R_OK) != 0) {
        fprintf(stderr, "nob: no readable nob.h found; run `nob init` first\n");
        return 1;
    }

    if (needs_rebuild() && build_recipe() != 0) return 1;

    char **recipe_arguments = calloc((size_t)argument_count + 2, sizeof(*recipe_arguments));
    if (recipe_arguments == NULL) {
        fprintf(stderr, "nob: out of memory\n");
        return 1;
    }

    recipe_arguments[0] = ".nob/nob";
    for (int index = 0; index < argument_count; ++index) {
        recipe_arguments[index + 1] = arguments[index];
    }

    execv(recipe_arguments[0], recipe_arguments);
    fprintf(stderr, "nob: could not run %s: %s\n", recipe_arguments[0], strerror(errno));
    free(recipe_arguments);
    return 1;
}

static int init_project(int argc, char **argv)
{
    bool force = false;
    const char *directory = ".";
    bool directory_was_set = false;

    for (int index = 0; index < argc; ++index) {
        if (strcmp(argv[index], "--force") == 0) {
            force = true;
        } else if (!directory_was_set) {
            directory = argv[index];
            directory_was_set = true;
        } else {
            fprintf(stderr, "nob: init accepts at most one directory\n");
            return 1;
        }
    }

    if (!is_directory(directory)) {
        fprintf(stderr, "nob: %s is not a directory\n", directory);
        return 1;
    }

    return copy_installed_header(directory, force);
}

int main(int argc, char **argv)
{
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("nob %s\n", NOB_VERSION);
        return 0;
    }

    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
                     strcmp(argv[1], "help") == 0)) {
        print_usage(stdout);
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "init") == 0) {
        return init_project(argc - 2, argv + 2);
    }

    if (argc > 1 && strcmp(argv[1], "run") == 0) {
        return run_recipe(argc - 2, argv + 2);
    }

    return run_recipe(argc - 1, argv + 1);
}
