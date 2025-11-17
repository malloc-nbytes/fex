#include <forge/colors.h>
#include <forge/ctrl.h>
#include <forge/err.h>
#include <forge/cmd.h>
#include <forge/arg.h>
#include <forge/io.h>
#include <forge/cstr.h>

#include <stdio.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

static void rm_file(const char *fp);

struct {
        uint32_t flags;
} g_config;

typedef struct {
        struct {
                struct termios t;
                size_t w;
                size_t h;
        } term;
        char *filepath;
} fex_context;

static void
selection_up(size_t *s)
{
        if (*s > 0) {
                --(*s);
        }
}

static void
selection_down(size_t *s, size_t n)
{
        if (*s < n-1) {
                ++(*s);
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

static void
display(fex_context *ctx)
{
        assert(ctx->filepath);

        if (!forge_ctrl_enable_raw_terminal(STDIN_FILENO, &ctx->term.t)) {
                forge_err("could not enable raw terminal");
        }

        size_t selection = 0;

        while (1) {
                forge_ctrl_clear_terminal();

                char **files = ls(ctx->filepath);
                size_t files_n = 0;
                if (!files) {
                        forge_err_wargs("could not list files in filepath: %s", ctx->filepath);
                }

                for (size_t i = 0; files[i]; ++i) {
                        if (i == selection) printf(INVERT);
                        printf("%s\n", files[i]);
                        if (i == selection) printf(RESET);
                        ++files_n;
                }

                printf("%zu %zu\n", selection, files_n);

                char ch;
                forge_ctrl_input_type ty = forge_ctrl_get_input(&ch);

                switch (ty) {
                case USER_INPUT_TYPE_ARROW: {
                        if (ch == DOWN_ARROW) {
                                selection_down(&selection, files_n);
                        } else if (ch == UP_ARROW) {
                                selection_up(&selection);
                        }
                } break;
                case USER_INPUT_TYPE_NORMAL: {
                        if      (ch == 'q') goto done;
                        else if (ch == 'd') rm_file(files[selection]);
                } break;
                default: break;
                }

                for (size_t i = 0; files[i]; ++i) {
                        free(files[i]);
                }
                free(files);
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
                .filepath = filepath,
        };

        display(&ctx);

        return 0;
}
