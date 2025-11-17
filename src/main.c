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
#include <forge/rdln.h>
#include <forge/utils.h>
#include <forge/chooser.h>

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
        const char *last_query;
        size_t hoffset;
} fex_context;

static void minisleep(void) { usleep(800000/2); }

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
remove_selection(fex_context *ctx)
{
        str_array confirm = dyn_array_empty(str_array);
        size_t_array indices = dyn_array_empty(size_t_array);

        if (sizet_set_size(&ctx->marked) > 0) {
                size_t **ar = sizet_set_iter(&ctx->marked);
                for (size_t i = 0; ar[i]; ++i) {
                        char *path = ctx->selection.files.data[*ar[i]];
                        if (!strcmp(path, "..") || !strcmp(path, ".")) {
                                continue;
                        }
                        dyn_array_append(confirm, path);
                        dyn_array_append(indices, *ar[i]);
                }
                free(ar);
        } else {
                char *path = ctx->selection.files.data[ctx->selection.i];
                if (!strcmp(path, "..") || !strcmp(path, ".")) {
                        return;
                }
                dyn_array_append(confirm, path);
        }

        forge_ctrl_clear_terminal();
        for (size_t i = 0; i < confirm.len; ++i) {
                printf(RED BOLD "--- %s" RESET "\n", confirm.data[i]);
        }

        int choice = forge_chooser_yesno("Remove these files?", NULL, 1);
        if (choice) {
                for (size_t i = 0; i < confirm.len; ++i) {
                        rm_file(confirm.data[i]);
                        if (indices.len > 0) {
                                sizet_set_remove(&ctx->marked, indices.data[i]);
                        }
                }
        }

        dyn_array_free(confirm);
}

static void
clearln(fex_context *ctx)
{
        for (size_t i = 0; i < ctx->term.w; ++i) putchar(' ');
        forge_ctrl_cursor_to_col(1);
}

static int
rename_selection(fex_context *ctx)
{
        CURSOR_UP(1);
        clearln(ctx);
        printf(BOLD WHITE "--- Rename ---" RESET);

        const char *path = ctx->selection.files.data[ctx->selection.i];

        forge_ctrl_cursor_to_first_line();
        CURSOR_DOWN(ctx->selection.i + 1);
        forge_ctrl_cursor_to_col(strlen(path)+1);

        char *s = forge_rdln(NULL);

        if (!s || strlen(s) == 0) return 0;

        if (rename(path, s) != 0) {
                forge_err_wargs("failed to rename `%s` to `%s`", path, s);
        }

        return 1;
}

static void
search(fex_context *ctx,
       int          jmp,
       int          rev)
{
        NOOP(rev);

        if (!jmp) {
                CURSOR_UP(1);
                ctx->last_query = forge_rdln("Query: ");
        } else if (!ctx->last_query) {
                return;
        }

        if (!rev) {
                for (size_t i = ctx->selection.i+1; i < ctx->selection.files.len; ++i) {
                        if (forge_utils_regex(ctx->last_query, ctx->selection.files.data[i])) {
                                ctx->selection.i = i;
                                break;
                        }
                }
        } else {
                for (size_t i = ctx->selection.i-1; i > 0; --i) {
                        if (forge_utils_regex(ctx->last_query, ctx->selection.files.data[i])) {
                                ctx->selection.i = i;
                                break;
                        }
                }
        }
}

static int
ctrl_x(fex_context *ctx)
{
        char ch;
        forge_ctrl_input_type ty = forge_ctrl_get_input(&ch);

        if (ty == USER_INPUT_TYPE_NORMAL && ch == '\n') {
                return cd_selection(ctx, "..");
        } else if (ty == USER_INPUT_TYPE_CTRL && ch == CTRL_Q) {
                return rename_selection(ctx);
        }
 bad:
        CURSOR_UP(1);
        clearln(ctx);
        printf(INVERT BOLD RED "C-x: Unknown Sequence" RESET "\n");
        minisleep();
        return 0;
}

static int
is_like_compar(const void *a,
               const void *b)
{
        const char *const *pa = a;
        const char *const *pb = b;
        const char *na = *pa;
        const char *nb = *pb;

        if (strcmp(na, ".") == 0)  return -1;
        if (strcmp(nb, ".") == 0)  return  1;

        if (strcmp(na, "..") == 0) return -1;
        if (strcmp(nb, "..") == 0) return  1;

        return strcmp(na, nb);
}

static void
mark_or_unmark_selection(fex_context *ctx, int mark)
{
        if (ctx->selection.i == 0) {
                for (size_t i = 2; i < ctx->selection.files.len; ++i) {
                        if (!mark && sizet_set_contains(&ctx->marked, i)) {
                                sizet_set_remove(&ctx->marked, i);
                        } else if (mark && !sizet_set_contains(&ctx->marked, i)) {
                                sizet_set_insert(&ctx->marked, i);
                        }
                }
        } else if (ctx->selection.i != 1) {
                if (!mark && sizet_set_contains(&ctx->marked, ctx->selection.i)) {
                        sizet_set_remove(&ctx->marked, ctx->selection.i);
                } else if (mark && !sizet_set_contains(&ctx->marked, ctx->selection.i)) {
                        sizet_set_insert(&ctx->marked, ctx->selection.i);
                }
                selection_down(ctx);
        }

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

        int fs_changed  = 1;
        char **files    = NULL;

        while (1) {
                forge_ctrl_clear_terminal();

                if (fs_changed) {
                        files = ls(ctx->filepath);
                        if (!files) {
                                forge_err_wargs("could not list files in filepath: %s", ctx->filepath);
                        }
                        fs_changed = 0;

                        // Sort files
                        size_t count = 0;
                        while (files[count]) ++count;
                        qsort(files, count, sizeof(*files), is_like_compar);
                        for (size_t i = 0; i < count; ++i) {
                                dyn_array_append(ctx->selection.files, files[i]);
                        }
                }

                // If we are out-of-bounds (from deleting, marking, etc.) move
                // to valid location.
                while (ctx->selection.i > ctx->selection.files.len-1) {
                        --ctx->selection.i;
                }

                // Header
                char *abspath = forge_io_resolve_absolute_path(ctx->filepath);
                printf("Directory listing for " BOLD WHITE "%s" RESET "\n", abspath);
                free(abspath);


                // Print files
                size_t dirs_n = 0;
                size_t start = ctx->hoffset;
                size_t end = start + ctx->term.h - 2;
                if (end > ctx->selection.files.len)
                        end = ctx->selection.files.len;
                for (size_t i = start; i < end; ++i) {
                        int is_selected = i == ctx->selection.i;

                        if (forge_io_is_dir(files[i])) {
                                printf(BOLD);
                                ++dirs_n;
                        } else {
                                printf(YELLOW);
                        }

                        if (is_selected) {
                                printf(INVERT);
                        }
                        if (sizet_set_contains(&ctx->marked, i)) {
                                printf(PINK);
                                printf("<M> ");
                        }

                        printf("%s", ctx->selection.files.data[i]);

                        if (is_selected) {
                                char *abspath = forge_io_resolve_absolute_path(ctx->selection.files.data[i]);
                                printf(RESET "  " GRAY "%s" RESET "\n", abspath);
                                free(abspath);
                        } else {
                                putchar('\n');
                        }

                        printf(RESET);
                }

                // Directory status
                printf(BOLD WHITE "%zu files, %zu directories" RESET " [" YELLOW "%zu" RESET "/" YELLOW "%zu" RESET "]\n",
                       ctx->selection.files.len - dirs_n, dirs_n - 2, ctx->selection.i+1, ctx->selection.files.len);

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
                        else if (ch == CTRL_X) fs_changed = ctrl_x(ctx);
                } break;
                case USER_INPUT_TYPE_NORMAL: {
                        if      (ch == 'q') goto done;
                        else if (ch == 'd') {
                                remove_selection(ctx);
                                fs_changed = 1;
                        }
                        else if (ch == 'j') selection_down(ctx);
                        else if (ch == 'k') selection_up(ctx);
                        else if (ch == 'r') {
                                fs_changed = rename_selection(ctx);
                        }
                        else if (ch == '\n') {
                                if (cd_selection(ctx, files[ctx->selection.i])) {
                                        ctx->selection.i = 0;
                                        fs_changed = 1;
                                }
                        } else if (ch == 'm') {
                                mark_or_unmark_selection(ctx, /*mark=*/1);
                        } else if (ch == 'u') {
                                mark_or_unmark_selection(ctx, /*mark=*/0);
                        } else if (ch == '/') {
                                search(ctx, /*jmp=*/0, /*rev=*/0);
                        } else if (ch == 'n') {
                                search(ctx, /*jmp=*/1, /*rev=*/0);
                        } else if (ch == 'N') {
                                search(ctx, /*jmp=*/1, /*rev=*/1);
                        }
                } break;
                default: break;
                }

                size_t visible_lines = ctx->term.h - 2;  // -2 for path + status line

                // Scroll down when selection reaches bottom of screen
                if (ctx->selection.i >= ctx->hoffset + visible_lines) {
                        ctx->hoffset = ctx->selection.i - visible_lines + 1;
                }

                // Scroll up when selection reaches top of screen
                if (ctx->selection.i < ctx->hoffset) {
                        ctx->hoffset = ctx->selection.i;
                }

                // Clamp hoffset to valid range
                if (ctx->hoffset + visible_lines > ctx->selection.files.len) {
                        ctx->hoffset = ctx->selection.files.len > visible_lines ?
                                ctx->selection.files.len - visible_lines : 0;
                }
                if (ctx->hoffset >= ctx->selection.files.len) {
                        ctx->hoffset = 0;
                }

                if (fs_changed) {
                        for (size_t i = 0; files[i]; ++i) {
                                free(files[i]);
                        }
                        free(files);
                        dyn_array_clear(ctx->selection.files);
                        ctx->last_query = NULL;
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
                        filepath = forge_io_resolve_absolute_path(arg->s);
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
                .marked = sizet_set_create(sizet_hash, sizet_cmp, NULL),
                .last_query = NULL,
                .hoffset = 0,
        };

        display(&ctx);

        return 0;
}
