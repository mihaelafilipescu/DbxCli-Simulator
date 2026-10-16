/*
 * dbxcli_shell.c - Interactive shell simulating dbxcli functionality
 * Operates on local filesystem
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <ftw.h>

// Color codes
#define COLOR_RED     "\033[0;31m"
#define COLOR_GREEN   "\033[0;32m"
#define COLOR_BLUE    "\033[0;34m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_RESET   "\033[0m"

// Global variables
static char current_dir[PATH_MAX];
static char home_dir[PATH_MAX];
// Simulated Dropbox (remote) root and current directory
static char dropbox_root[PATH_MAX];
static char dropbox_dir[PATH_MAX];

// Function prototypes
void show_error(const char *msg);
void show_success(const char *msg);
void show_info(const char *msg);
void show_help(void);
int resolve_path(const char *path, char *resolved);
int resolve_dbx_path(const char *path, char *resolved);
int ensure_under_root(const char *root, const char *path);
void dbx_relpath(char *out, size_t out_sz);
int cmd_pwd(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_ls(int argc, char **argv);
int cmd_mkdir(int argc, char **argv);
int cmd_rm(int argc, char **argv);
int cmd_cp(int argc, char **argv);
int cmd_mv(int argc, char **argv);
// Dropbox (simulated) commands
int cmd_dbx_pwd(int argc, char **argv);
int cmd_dbx_cd(int argc, char **argv);
int cmd_dbx_ls(int argc, char **argv);
int cmd_dbx_mkdir(int argc, char **argv);
int cmd_dbx_rm(int argc, char **argv);
int cmd_dbx_cp(int argc, char **argv);
int cmd_dbx_mv(int argc, char **argv);
// Link local <-> dropbox
int cmd_put(int argc, char **argv);
int cmd_get(int argc, char **argv);
int dispatch_command(int argc, char **argv);

// Utility functions
void show_error(const char *msg) {
    fprintf(stderr, "%sError: %s%s\n", COLOR_RED, msg, COLOR_RESET);
}

void show_success(const char *msg) {
    printf("%s%s%s\n", COLOR_GREEN, msg, COLOR_RESET);
}

void show_info(const char *msg) {
    printf("%s%s%s\n", COLOR_BLUE, msg, COLOR_RESET);
}

void show_help(void) {
    printf("dbxcli shell - Commands:\n\n");
    printf("Navigation and Listing:\n");
    printf("  pwd                   - Print current working directory\n");
    printf("  cd <path>            - Change current directory\n");
    printf("  ls [path]            - List directory contents\n\n");
    printf("File Operations:\n");
    printf("  mkdir <path>         - Create a new directory\n");
    printf("  rm <path>            - Remove file or directory\n");
    printf("  cp <source> <dest>   - Copy file or directory\n");
    printf("  mv <source> <dest>   - Move/rename file or directory\n\n");

    printf("\nSimulated Dropbox (remote) Operations:\n");
    printf("  dbx pwd               - Print current Dropbox directory\n");
    printf("  dbx cd <path>         - Change Dropbox directory\n");
    printf("  dbx ls [path]         - List Dropbox directory contents\n");
    printf("  dbx mkdir <path>      - Create Dropbox directory\n");
    printf("  dbx rm <path>         - Remove Dropbox file or directory\n");
    printf("  dbx cp <src> <dst>    - Copy inside Dropbox\n");
    printf("  dbx mv <src> <dst>    - Move/rename inside Dropbox\n");
    printf("  put <local> <dbx>     - Upload from local FS to Dropbox\n");
    printf("  get <dbx> <local>     - Download from Dropbox to local FS\n\n");
    printf("Utility:\n");
    printf("  help                 - Show this help message\n");
    printf("  exit, quit           - Exit the shell\n\n");
    printf("Notes:\n");
    printf("  - Supports both absolute and relative paths\n");
    printf("  - Use ~ for home directory\n");
    printf("  - Use . for current directory, .. for parent directory\n");
    printf("  - Dropbox paths are interpreted inside a simulated remote root\n");
    printf("    located at: %s\n", dropbox_root);
}

// Ensure "path" is within "root" after normalization.
// Returns 0 if OK, -1 otherwise.
int ensure_under_root(const char *root, const char *path) {
    size_t rlen = strlen(root);
    if (strncmp(root, path, rlen) != 0) return -1;
    // Allow exact match or root + '/'
    if (path[rlen] == '\0' || path[rlen] == '/') return 0;
    return -1;
}

// Get Dropbox path for prompt (relative to dropbox_root)
void dbx_relpath(char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    size_t rlen = strlen(dropbox_root);
    if (strncmp(dropbox_dir, dropbox_root, rlen) == 0) {
        const char *rel = dropbox_dir + rlen;
        if (*rel == '\0') {
            snprintf(out, out_sz, "/");
        } else {
            snprintf(out, out_sz, "%s", rel);
        }
    } else {
        // Should never happen if we validate paths
        snprintf(out, out_sz, "?");
    }
}

// Resolve path (handle absolute, relative, ~)
int resolve_path(const char *path, char *resolved) {
    if (!path || !resolved) {
        return -1;
    }

    // Handle empty path
    if (strlen(path) == 0) {
        strcpy(resolved, current_dir);
        return 0;
    }

    // Handle absolute path
    if (path[0] == '/') {
        strcpy(resolved, path);
        return 0;
    }

    // Handle ~ expansion
    if (path[0] == '~') {
        if (strlen(path) == 1) {
            strcpy(resolved, home_dir);
        } else {
            snprintf(resolved, PATH_MAX, "%s%s", home_dir, path + 1);
        }
        return 0;
    }

    // Handle . (current directory)
    if (strcmp(path, ".") == 0) {
        strcpy(resolved, current_dir);
        return 0;
    }

    // Handle .. (parent directory)
    if (strcmp(path, "..") == 0) {
        strcpy(resolved, current_dir);
        char *last_slash = strrchr(resolved, '/');
        if (last_slash && last_slash != resolved) {
            *last_slash = '\0';
        } else if (last_slash == resolved) {
            strcpy(resolved, "/");
        }
        return 0;
    }

    // Handle relative path
    snprintf(resolved, PATH_MAX, "%s/%s", current_dir, path);
    return 0;
}

// Resolve a Dropbox path (inside simulated remote root)
// Rules:
//  - "/" is Dropbox root
//  - Absolute-like paths starting with "/" are relative to Dropbox root
//  - Relative paths are relative to current Dropbox directory
//  - ".." won't be allowed to escape Dropbox root (checked later via realpath)
int resolve_dbx_path(const char *path, char *resolved) {
    if (!path || !resolved) return -1;

    if (strlen(path) == 0) {
        strcpy(resolved, dropbox_dir);
        return 0;
    }

    if (strcmp(path, "/") == 0) {
        strcpy(resolved, dropbox_root);
        return 0;
    }

    // Treat leading '/' as Dropbox-rooted path
    if (path[0] == '/') {
        snprintf(resolved, PATH_MAX, "%s%s", dropbox_root, path);
        return 0;
    }

    if (strcmp(path, ".") == 0) {
        strcpy(resolved, dropbox_dir);
        return 0;
    }

    if (strcmp(path, "..") == 0) {
        strcpy(resolved, dropbox_dir);
        char *last_slash = strrchr(resolved, '/');
        if (last_slash && strcmp(resolved, dropbox_root) != 0) {
            *last_slash = '\0';
            if (strlen(resolved) < strlen(dropbox_root)) {
                strcpy(resolved, dropbox_root);
            }
        }
        return 0;
    }

    snprintf(resolved, PATH_MAX, "%s/%s", dropbox_dir, path);
    return 0;
}

// Command: pwd
int cmd_pwd(int argc, char **argv) {
    printf("%s\n", current_dir);
    return 0;
}

// Command: cd
int cmd_cd(int argc, char **argv) {
    char resolved[PATH_MAX];
    char *target;

    if (argc < 2) {
        // No argument, go to home
        strcpy(current_dir, home_dir);
        return 0;
    }

    target = argv[1];

    if (resolve_path(target, resolved) != 0) {
        show_error("Failed to resolve path");
        return -1;
    }

    // Check if directory exists
    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Directory not found: %s", target);
        show_error(errmsg);
        return -1;
    }

    // Normalize path using realpath
    char real[PATH_MAX];
    if (realpath(resolved, real) == NULL) {
        show_error("Failed to resolve real path");
        return -1;
    }

    strcpy(current_dir, real);
    return 0;
}

// Command: ls
int cmd_ls(int argc, char **argv) {
    char resolved[PATH_MAX];
    char *target = (argc < 2) ? current_dir : argv[1];

    if (argc >= 2) {
        if (resolve_path(target, resolved) != 0) {
            show_error("Failed to resolve path");
            return -1;
        }
    } else {
        strcpy(resolved, current_dir);
    }

    // Check if path exists
    struct stat st;
    if (stat(resolved, &st) != 0) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Path not found: %s", target);
        show_error(errmsg);
        return -1;
    }

    // Use system ls command for better formatting
    char cmd[PATH_MAX + 20];
    snprintf(cmd, sizeof(cmd), "ls -lh --color=auto '%s'", resolved);
    return system(cmd);
}

// Command: mkdir
int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) {
        show_error("mkdir requires a path argument");
        printf("Usage: mkdir <path>\n");
        return -1;
    }

    char resolved[PATH_MAX];
    if (resolve_path(argv[1], resolved) != 0) {
        show_error("Failed to resolve path");
        return -1;
    }

    // Create directory with parents
    char cmd[PATH_MAX + 20];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", resolved);

    if (system(cmd) == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Directory created: %s", argv[1]);
        show_success(msg);
        return 0;
    } else {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Failed to create directory: %s", argv[1]);
        show_error(errmsg);
        return -1;
    }
}

// Command: rm
int cmd_rm(int argc, char **argv) {
    if (argc < 2) {
        show_error("rm requires a path argument");
        printf("Usage: rm <path>\n");
        return -1;
    }

    char resolved[PATH_MAX];
    if (resolve_path(argv[1], resolved) != 0) {
        show_error("Failed to resolve path");
        return -1;
    }

    // Check if path exists
    struct stat st;
    if (stat(resolved, &st) != 0) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Path not found: %s", argv[1]);
        show_error(errmsg);
        return -1;
    }

    // Ask for confirmation
    printf("Remove '%s'? (y/n): ", argv[1]);
    fflush(stdout);

    char response[10];
    if (fgets(response, sizeof(response), stdin) == NULL) {
        return -1;
    }

    if (response[0] != 'y' && response[0] != 'Y') {
        printf("Cancelled.\n");
        return 0;
    }

    // Remove file or directory
    char cmd[PATH_MAX + 20];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", resolved);

    if (system(cmd) == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Removed: %s", argv[1]);
        show_success(msg);
        return 0;
    } else {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Failed to remove: %s", argv[1]);
        show_error(errmsg);
        return -1;
    }
}

// Command: cp
int cmd_cp(int argc, char **argv) {
    if (argc < 3) {
        show_error("cp requires source and destination arguments");
        printf("Usage: cp <source> <destination>\n");
        return -1;
    }

    char resolved_src[PATH_MAX];
    char resolved_dst[PATH_MAX];

    if (resolve_path(argv[1], resolved_src) != 0) {
        show_error("Failed to resolve source path");
        return -1;
    }

    if (resolve_path(argv[2], resolved_dst) != 0) {
        show_error("Failed to resolve destination path");
        return -1;
    }

    // Check if source exists
    struct stat st;
    if (stat(resolved_src, &st) != 0) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Source not found: %s", argv[1]);
        show_error(errmsg);
        return -1;
    }

    // Copy file or directory
    char cmd[PATH_MAX * 2 + 20];
    snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'", resolved_src, resolved_dst);

    if (system(cmd) == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Copied: %s -> %s", argv[1], argv[2]);
        show_success(msg);
        return 0;
    } else {
        char errmsg[512];
        snprintf(errmsg, sizeof(errmsg), "Failed to copy: %s -> %s", argv[1], argv[2]);
        show_error(errmsg);
        return -1;
    }
}

// Command: mv
int cmd_mv(int argc, char **argv) {
    if (argc < 3) {
        show_error("mv requires source and destination arguments");
        printf("Usage: mv <source> <destination>\n");
        return -1;
    }

    char resolved_src[PATH_MAX];
    char resolved_dst[PATH_MAX];

    if (resolve_path(argv[1], resolved_src) != 0) {
        show_error("Failed to resolve source path");
        return -1;
    }

    if (resolve_path(argv[2], resolved_dst) != 0) {
        show_error("Failed to resolve destination path");
        return -1;
    }

    // Check if source exists
    struct stat st;
    if (stat(resolved_src, &st) != 0) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Source not found: %s", argv[1]);
        show_error(errmsg);
        return -1;
    }

    // Move/rename file or directory
    if (rename(resolved_src, resolved_dst) == 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Moved: %s -> %s", argv[1], argv[2]);
        show_success(msg);
        return 0;
    } else {
        // Try with system mv command (for cross-filesystem moves)
        char cmd[PATH_MAX * 2 + 20];
        snprintf(cmd, sizeof(cmd), "mv '%s' '%s'", resolved_src, resolved_dst);

        if (system(cmd) == 0) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Moved: %s -> %s", argv[1], argv[2]);
            show_success(msg);
            return 0;
        } else {
            char errmsg[512];
            snprintf(errmsg, sizeof(errmsg), "Failed to move: %s -> %s", argv[1], argv[2]);
            show_error(errmsg);
            return -1;
        }
    }
}

// ---------------------------
// Dropbox (simulated) commands
// ---------------------------
int cmd_dbx_pwd(int argc, char **argv) {
    char rel[PATH_MAX];
    dbx_relpath(rel, sizeof(rel));
    printf("%s\n", rel);
    return 0;
}

int cmd_dbx_cd(int argc, char **argv) {
    char resolved[PATH_MAX];

    if (argc < 3) {
        // "dbx cd" with no arg -> root
        strcpy(dropbox_dir, dropbox_root);
        return 0;
    }

    if (resolve_dbx_path(argv[2], resolved) != 0) {
        show_error("Failed to resolve Dropbox path");
        return -1;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Dropbox directory not found: %s", argv[2]);
        show_error(errmsg);
        return -1;
    }

    char real[PATH_MAX];
    if (realpath(resolved, real) == NULL) {
        show_error("Failed to resolve Dropbox real path");
        return -1;
    }
    if (ensure_under_root(dropbox_root, real) != 0) {
        show_error("Access denied: cannot leave Dropbox root");
        return -1;
    }

    strcpy(dropbox_dir, real);
    return 0;
}

int cmd_dbx_ls(int argc, char **argv) {
    char resolved[PATH_MAX];
    const char *target = (argc < 3) ? dropbox_dir : argv[2];

    if (argc >= 3) {
        if (resolve_dbx_path(target, resolved) != 0) {
            show_error("Failed to resolve Dropbox path");
            return -1;
        }
    } else {
        strcpy(resolved, dropbox_dir);
    }

    struct stat st;
    if (stat(resolved, &st) != 0) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Dropbox path not found: %s", target);
        show_error(errmsg);
        return -1;
    }

    char real[PATH_MAX];
    if (realpath(resolved, real) == NULL) {
        show_error("Failed to resolve Dropbox real path");
        return -1;
    }
    if (ensure_under_root(dropbox_root, real) != 0) {
        show_error("Access denied: cannot leave Dropbox root");
        return -1;
    }

    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "ls -lh --color=auto '%s'", real);
    return system(cmd);
}

int cmd_dbx_mkdir(int argc, char **argv) {
    if (argc < 3) {
        show_error("dbx mkdir requires a path argument");
        printf("Usage: dbx mkdir <path>\n");
        return -1;
    }

    char resolved[PATH_MAX];
    if (resolve_dbx_path(argv[2], resolved) != 0) {
        show_error("Failed to resolve Dropbox path");
        return -1;
    }

    // Create with parents
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", resolved);
    if (system(cmd) != 0) {
        show_error("Failed to create Dropbox directory");
        return -1;
    }

    char real[PATH_MAX];
    if (realpath(resolved, real) == NULL) {
        // Directory might have been created but realpath failed unexpectedly
        show_error("Failed to resolve Dropbox directory real path");
        return -1;
    }
    if (ensure_under_root(dropbox_root, real) != 0) {
        show_error("Access denied: cannot leave Dropbox root");
        return -1;
    }

    show_success("Dropbox directory created");
    return 0;
}

int cmd_dbx_rm(int argc, char **argv) {
    if (argc < 3) {
        show_error("dbx rm requires a path argument");
        printf("Usage: dbx rm <path>\n");
        return -1;
    }

    char resolved[PATH_MAX];
    if (resolve_dbx_path(argv[2], resolved) != 0) {
        show_error("Failed to resolve Dropbox path");
        return -1;
    }

    char real[PATH_MAX];
    if (realpath(resolved, real) == NULL) {
        show_error("Dropbox path not found");
        return -1;
    }
    if (ensure_under_root(dropbox_root, real) != 0) {
        show_error("Access denied: cannot leave Dropbox root");
        return -1;
    }

    printf("Remove Dropbox '%s'? (y/n): ", argv[2]);
    fflush(stdout);
    char response[10];
    if (fgets(response, sizeof(response), stdin) == NULL) return -1;
    if (response[0] != 'y' && response[0] != 'Y') {
        printf("Cancelled.\n");
        return 0;
    }

    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", real);
    if (system(cmd) == 0) {
        show_success("Removed from Dropbox");
        return 0;
    }

    show_error("Failed to remove from Dropbox");
    return -1;
}

int cmd_dbx_cp(int argc, char **argv) {
    if (argc < 4) {
        show_error("dbx cp requires source and destination");
        printf("Usage: dbx cp <source> <destination>\n");
        return -1;
    }

    char src[PATH_MAX], dst[PATH_MAX];
    if (resolve_dbx_path(argv[2], src) != 0 || resolve_dbx_path(argv[3], dst) != 0) {
        show_error("Failed to resolve Dropbox path");
        return -1;
    }

    char real_src[PATH_MAX];
    if (realpath(src, real_src) == NULL) {
        show_error("Dropbox source not found");
        return -1;
    }
    if (ensure_under_root(dropbox_root, real_src) != 0) {
        show_error("Access denied: cannot leave Dropbox root");
        return -1;
    }

    // Destination might not exist, validate parent after normalization
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'", real_src, dst);
    if (system(cmd) == 0) {
        show_success("Copied inside Dropbox");
        return 0;
    }
    show_error("Failed to copy inside Dropbox");
    return -1;
}

int cmd_dbx_mv(int argc, char **argv) {
    if (argc < 4) {
        show_error("dbx mv requires source and destination");
        printf("Usage: dbx mv <source> <destination>\n");
        return -1;
    }

    char src[PATH_MAX], dst[PATH_MAX];
    if (resolve_dbx_path(argv[2], src) != 0 || resolve_dbx_path(argv[3], dst) != 0) {
        show_error("Failed to resolve Dropbox path");
        return -1;
    }

    char real_src[PATH_MAX];
    if (realpath(src, real_src) == NULL) {
        show_error("Dropbox source not found");
        return -1;
    }
    if (ensure_under_root(dropbox_root, real_src) != 0) {
        show_error("Access denied: cannot leave Dropbox root");
        return -1;
    }

    if (rename(real_src, dst) == 0) {
        show_success("Moved inside Dropbox");
        return 0;
    }

    // Fallback
    char cmd[PATH_MAX * 2 + 32];
    snprintf(cmd, sizeof(cmd), "mv '%s' '%s'", real_src, dst);
    if (system(cmd) == 0) {
        show_success("Moved inside Dropbox");
        return 0;
    }
    show_error("Failed to move inside Dropbox");
    return -1;
}

// Upload local -> Dropbox
int cmd_put(int argc, char **argv) {
    if (argc < 3) {
        show_error("put requires <local_path> <dropbox_path>");
        printf("Usage: put <local_path> <dropbox_path>\n");
        return -1;
    }
    char local_src[PATH_MAX];
    char dbx_dst[PATH_MAX];
    if (resolve_path(argv[1], local_src) != 0) {
        show_error("Failed to resolve local path");
        return -1;
    }
    if (resolve_dbx_path(argv[2], dbx_dst) != 0) {
        show_error("Failed to resolve Dropbox path");
        return -1;
    }

    struct stat st;
    if (stat(local_src, &st) != 0) {
        show_error("Local source not found");
        return -1;
    }

    char cmd[PATH_MAX * 2 + 64];
    snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'", local_src, dbx_dst);
    if (system(cmd) == 0) {
        show_success("Uploaded to Dropbox");
        return 0;
    }
    show_error("Upload failed");
    return -1;
}

// Download Dropbox -> local
int cmd_get(int argc, char **argv) {
    if (argc < 3) {
        show_error("get requires <dropbox_path> <local_path>");
        printf("Usage: get <dropbox_path> <local_path>\n");
        return -1;
    }
    char dbx_src[PATH_MAX];
    char local_dst[PATH_MAX];
    if (resolve_dbx_path(argv[1], dbx_src) != 0) {
        show_error("Failed to resolve Dropbox path");
        return -1;
    }
    if (resolve_path(argv[2], local_dst) != 0) {
        show_error("Failed to resolve local path");
        return -1;
    }

    char real_dbx_src[PATH_MAX];
    if (realpath(dbx_src, real_dbx_src) == NULL) {
        show_error("Dropbox source not found");
        return -1;
    }
    if (ensure_under_root(dropbox_root, real_dbx_src) != 0) {
        show_error("Access denied: cannot leave Dropbox root");
        return -1;
    }

    char cmd[PATH_MAX * 2 + 64];
    snprintf(cmd, sizeof(cmd), "cp -r '%s' '%s'", real_dbx_src, local_dst);
    if (system(cmd) == 0) {
        show_success("Downloaded from Dropbox");
        return 0;
    }
    show_error("Download failed");
    return -1;
}

// Command dispatcher
int dispatch_command(int argc, char **argv) {
    if (argc == 0 || strlen(argv[0]) == 0) {
        return 0;
    }

    const char *cmd = argv[0];

    if (strcmp(cmd, "pwd") == 0) {
        return cmd_pwd(argc, argv);
    } else if (strcmp(cmd, "cd") == 0) {
        return cmd_cd(argc, argv);
    } else if (strcmp(cmd, "ls") == 0) {
        return cmd_ls(argc, argv);
    } else if (strcmp(cmd, "mkdir") == 0) {
        return cmd_mkdir(argc, argv);
    } else if (strcmp(cmd, "rm") == 0) {
        return cmd_rm(argc, argv);
    } else if (strcmp(cmd, "cp") == 0) {
        return cmd_cp(argc, argv);
    } else if (strcmp(cmd, "mv") == 0) {
        return cmd_mv(argc, argv);
    } else if (strcmp(cmd, "dbx") == 0) {
        // Subcommands: pwd/cd/ls/mkdir/rm/cp/mv
        if (argc < 2) {
            show_error("Usage: dbx <subcommand> [args...]");
            return -1;
        }
        if (strcmp(argv[1], "pwd") == 0) return cmd_dbx_pwd(argc, argv);
        if (strcmp(argv[1], "cd") == 0) return cmd_dbx_cd(argc, argv);
        if (strcmp(argv[1], "ls") == 0) return cmd_dbx_ls(argc, argv);
        if (strcmp(argv[1], "mkdir") == 0) return cmd_dbx_mkdir(argc, argv);
        if (strcmp(argv[1], "rm") == 0) return cmd_dbx_rm(argc, argv);
        if (strcmp(argv[1], "cp") == 0) return cmd_dbx_cp(argc, argv);
        if (strcmp(argv[1], "mv") == 0) return cmd_dbx_mv(argc, argv);

        show_error("Unknown dbx subcommand. Try: dbx ls | dbx cd | dbx pwd | dbx mkdir | dbx rm | dbx cp | dbx mv");
        return -1;
    } else if (strcmp(cmd, "put") == 0) {
        return cmd_put(argc, argv);
    } else if (strcmp(cmd, "get") == 0) {
        return cmd_get(argc, argv);
    } else if (strcmp(cmd, "help") == 0) {
        show_help();
        return 0;
    } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
        printf("Goodbye!\n");
        exit(0);
    } else {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), "Unknown command: %s", cmd);
        show_error(errmsg);
        printf("Type 'help' for available commands.\n");
        return -1;
    }
}

// Parse input line into argc/argv
int parse_input(char *input, char **argv, int max_args) {
    int argc = 0;
    char *token;
    char *saveptr;

    // Remove trailing newline
    size_t len = strlen(input);
    if (len > 0 && input[len - 1] == '\n') {
        input[len - 1] = '\0';
    }

    // Skip leading whitespace
    while (*input && (*input == ' ' || *input == '\t')) {
        input++;
    }

    // Tokenize
    token = strtok_r(input, " \t", &saveptr);
    while (token != NULL && argc < max_args - 1) {
        argv[argc++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }
    argv[argc] = NULL;

    return argc;
}

// Main function
int main(int argc, char *argv[]) {
    // Initialize current directory
    if (getcwd(current_dir, sizeof(current_dir)) == NULL) {
        perror("getcwd");
        return 1;
    }

    // Initialize home directory
    const char *home = getenv("HOME");
    if (home) {
        strncpy(home_dir, home, sizeof(home_dir) - 1);
    } else {
        strcpy(home_dir, "/");
    }

    // Initialize simulated Dropbox root under HOME
    snprintf(dropbox_root, sizeof(dropbox_root), "%s/.dropbox_sim", home_dir);
    // Create it if missing
    char mkcmd[PATH_MAX + 32];
    snprintf(mkcmd, sizeof(mkcmd), "mkdir -p '%s'", dropbox_root);
    if (system(mkcmd) != 0) {
        show_error("Failed to initialize simulated Dropbox folder");
        return 1;
    }
    // Start in Dropbox root
    strcpy(dropbox_dir, dropbox_root);

    // Print welcome message
    show_info("=== dbxcli shell ===");
    printf("Type 'help' for available commands, 'exit' to quit.\n\n");

    // Main loop
    char input[1024];
    char *args[64];
    int arg_count;

    while (1) {
        // Print prompt
        char dbx_prompt[PATH_MAX];
        dbx_relpath(dbx_prompt, sizeof(dbx_prompt));
        printf("%sdbxcli%s:%s (dbx:%s)>%s ", COLOR_GREEN, COLOR_RESET, current_dir, dbx_prompt, COLOR_RESET);
        fflush(stdout);

        // Read input
        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("\nGoodbye!\n");
            break;
        }

        // Parse input
        arg_count = parse_input(input, args, 64);

        // Skip empty input
        if (arg_count == 0) {
            continue;
        }

        // Dispatch command
        dispatch_command(arg_count, args);
    }

    return 0;
}
