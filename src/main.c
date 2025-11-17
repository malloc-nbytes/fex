#include <forge/colors.h>
#include <forge/ctrl.h>
#include <forge/err.h>
#include <forge/cmd.h>
#include <forge/arg.h>
#include <forge/io.h>
#include <forge/cstr.h>
#include <forge/set.h>
#include <forge/logger.h>
#include <forge/viewer.h>
#include <forge/array.h>

#include <stdio.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

FORGE_SET_TYPE(size_t, sizet_set)

unsigned
sizet_hash(size_t *i)
{
        return *i;
}

int
sizet_cmp(size_t *x, size_t *y)
{
        return *x - *y;
}

static void rm_file(const char *fp);

struct {
        uint32_t flags;
} g_config;

forge_logger logger = {0};

typedef struct {
        struct {
                struct termios t;
                size_t w;
                size_t h;
        } term;
        struct {
                size_t i;
                str_array files;
        } selection;
        char *filepath;
        sizet_set marked;
} fex_context;

static void
selection_up(fex_context *ctx)
{
        if (ctx->selection.i > 0) {
                --ctx->selection.i;
        }
}

static void
selection_down(fex_context *ctx)
{
        if (ctx->selection.i < ctx->selection.files.len-1) {
                ++ctx->selection.i;
        }
}

static void
rm_dir(const char *fp)
{
        char **files = ls(fp);

        for (size_t i = 0; files && files[i]; ++i) {
                if (!strcmp(files[i], "..")) continue;
                if (!strcmp(files[i], "."))  continue;
                char *path = forge_cstr_builder(fp, "/", files[i], NULL);
                if (forge_io_is_dir(path)) {
                        rm_dir(path);
                } else {
                        rm_file(path);
                }
                free(path);
                free(files[i]);
        }

        if (remove(fp) != 0) {
                perror("remove");
                exit(1);
        }
}

static void
rm_file(const char *fp)
{
        if (forge_io_is_dir(fp)) {
                rm_dir(fp);
        } else {
                if (remove(fp) != 0) {
                        perror("remove");
                        exit(1);
                }
        }
}

static int
cd_selection(fex_context *ctx,
             const char  *to)
{
        if (forge_io_is_dir(to)) {
                free(ctx->filepath);
                ctx->filepath = forge_io_resolve_absolute_path(to);
                CD(ctx->filepath, forge_err_wargs("could not cd() to %s", ctx->filepath));
                return 1;
        } else {
                char **lns = forge_io_read_file_to_lines(to);
                size_t lns_n;
                for (lns_n = 0; lns[lns_n]; ++lns_n);
                forge_viewer *v = forge_viewer_alloc(lns, lns_n, 1);
                forge_viewer_display(v);
                forge_viewer_free(v);
                for (size_t i = 0; i < lns_n; ++i) free(lns[i]);
                free(lns);
        }
        return 0;
}

static void
display(fex_context *ctx)
{
        assert(ctx->filepath);

        CD(ctx->filepath, forge_err_wargs("could not cd() to %s", ctx->filepath));

        if (!forge_ctrl_enable_raw_terminal(STDIN_FILENO, &ctx->term.t)) {
                forge_err("could not enable raw terminal");
        }

        ctx->selection.i = 0;

        int changed_dir = 1;
        char **files    = NULL;

        while (1) {
                forge_ctrl_clear_terminal();

                forge_logger_log(&logger, FORGE_LOG_LEVEL_DEBUG, "directory: %s", ctx->filepath);

                if (changed_dir) {
                        files = ls(ctx->filepath);
                        if (!files) {
                                forge_err_wargs("could not list files in filepath: %s", ctx->filepath);
                        }
                        changed_dir = 0;

                        // Put . and .. in correct spots, count files.
                        for (size_t i = 0; files[i]; ++i) {
                                if (!strcmp(files[i], ".")) {
                                        char *tmp = files[0];
                                        files[0] = files[i];
                                        files[i] = tmp;
                                } else if (!strcmp(files[i], "..")) {
                                        char *tmp = files[1];
                                        files[1] = files[i];
                                        files[i] = tmp;
                                }
                                dyn_array_append(ctx->selection.files, files[i]);
                        }

                        ctx->selection.files.data[0] = files[0];
                        ctx->selection.files.data[1] = files[1];
                }

                char *abspath = forge_io_resolve_absolute_path(ctx->filepath);
                printf(YELLOW "%s" RESET ":\n", abspath);
                free(abspath);

                // Print files
                for (size_t i = 0; files[i]; ++i) {
                        printf("  ");

                        int should_reset = 0;

                        if (i == ctx->selection.i) {
                                printf(INVERT);
                                should_reset = 1;
                        }
                        if (sizet_set_contains(&ctx->marked, i)) {
                                printf(ORANGE);
                                should_reset = 1;
                        }

                        printf("%s\n", ctx->selection.files.data[i]);

                        if (should_reset) {
                                printf(RESET);
                        }
                }

                printf("%zu %zu\n", ctx->selection.i, ctx->selection.files.len);

                char ch;
                forge_ctrl_input_type ty = forge_ctrl_get_input(&ch);

                // Handle input
                switch (ty) {
                case USER_INPUT_TYPE_ARROW: {
                        if (ch == DOWN_ARROW) {
                                selection_down(ctx);
                        } else if (ch == UP_ARROW) {
                                selection_up(ctx);
                        }
                } break;
                case USER_INPUT_TYPE_CTRL: {
                        if      (ch == CTRL_N) selection_down(ctx);
                        else if (ch == CTRL_P) selection_up(ctx);
                } break;
                case USER_INPUT_TYPE_NORMAL: {
                        if      (ch == 'q') goto done;
                        else if (ch == 'd') {
                                rm_file(files[ctx->selection.i]);
                        }
                        else if (ch == 'j') selection_down(ctx);
                        else if (ch == 'k') selection_up(ctx);
                        else if (ch == '\n') {
                                if (cd_selection(ctx, files[ctx->selection.i])) {
                                        ctx->selection.i = 0;
                                        changed_dir = 1;
                                }
                        }
                        else if (ch == 'm') {
                                if (sizet_set_contains(&ctx->marked, ctx->selection.i)) {
                                        sizet_set_remove(&ctx->marked, ctx->selection.i);
                                } else {
                                        sizet_set_insert(&ctx->marked, ctx->selection.i);
                                }
                        }
                } break;
                default: break;
                }

                if (changed_dir) {
                        for (size_t i = 0; files[i]; ++i) {
                                free(files[i]);
                        }
                        free(files);
                        dyn_array_clear(ctx->selection.files);
                }
        }

 done:
        forge_ctrl_clear_terminal();

        if (!forge_ctrl_disable_raw_terminal(STDIN_FILENO, &ctx->term.t)) {
                forge_err("could not disable raw terminal");
        }
}

int
main(int argc, char **argv)
{
        if (!forge_logger_init(&logger, "./log", FORGE_LOG_LEVEL_DEBUG)) {
                forge_err("failed to init logger");
        }

        struct termios t;
        size_t w, h;
        char *filepath = NULL;

        forge_arg *arghd = forge_arg_alloc(argc, argv, 1);
        forge_arg *arg = arghd;
        while (arg) {
                if (arg->h == 1) {
                        forge_err("options are unimplemented");
                } else if (arg->h == 2) {
                        forge_err("options are unimplemented");
                } else {
                        filepath = strdup(arg->s);
                }
                arg = arg->n;
        }
        forge_arg_free(arghd);

        if (!filepath) filepath = cwd();

        if (!forge_ctrl_get_terminal_xy(&w, &h)) {
                forge_err("could not get the terminal size");
        }

        fex_context ctx = {
                .term = {
                        .t = t,
                        .w = w,
                        .h = h,
                },
                .selection = {
                        .i = 0,
                        .files = dyn_array_empty(str_array),
                },
                .filepath = filepath,
                .marked = sizet_set_create(sizet_hash, sizet_cmp, NULL)
        };

        display(&ctx);

        return 0;
}
