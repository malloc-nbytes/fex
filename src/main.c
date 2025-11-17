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
#include <limits.h>

#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <errno.h>

static void rm_file(const char *fp);

FORGE_SET_TYPE(size_t, sizet_set)

struct {
        uint32_t flags;
} g_config;

typedef struct {
        char        *name;
        struct stat  st;
        char        *owner;
        char        *group;
        int          stat_failed;
} FE;

DYN_ARRAY_TYPE(FE, FE_array);

typedef struct {
        struct {
                struct termios t;
                size_t w;
                size_t h;
        } term;
        struct {
                size_t i;
                FE_array fes;
        } entries;
        char *filepath;
        sizet_set marked;
        const char *last_query;
        size_t hoffset;
} fex_context;

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

static void minisleep(void) { usleep(800000/2); }

static void
mode_string(mode_t mode, char buf[11])
{
        strcpy(buf, "----------");

        if (S_ISDIR(mode))  buf[0] = 'd';
        else if (S_ISLNK(mode)) buf[0] = 'l';
        else if (S_ISBLK(mode)) buf[0] = 'b';
        else if (S_ISCHR(mode)) buf[0] = 'c';
        else if (S_ISFIFO(mode)) buf[0] = 'p';
        else if (S_ISSOCK(mode)) buf[0] = 's';

        if (mode & S_IRUSR) buf[1] = 'r';
        if (mode & S_IWUSR) buf[2] = 'w';
        if (mode & S_IXUSR) buf[3] = 'x';
        if (mode & S_IRGRP) buf[4] = 'r';
        if (mode & S_IWGRP) buf[5] = 'w';
        if (mode & S_IXGRP) buf[6] = 'x';
        if (mode & S_IROTH) buf[7] = 'r';
        if (mode & S_IWOTH) buf[8] = 'w';
        if (mode & S_IXOTH) buf[9] = 'x';

        // Sticky bit, setuid, setgid
        if (mode & S_ISUID) buf[3] = (mode & S_IXUSR) ? 's' : 'S';
        if (mode & S_ISGID) buf[6] = (mode & S_IXGRP) ? 's' : 'S';
        if (mode & S_ISVTX) buf[9] = (mode & S_IXOTH) ? 't' : 'T';
}

static const char *
human_size(off_t size)
{
        static char buf[32];
        if (size < 1024) snprintf(buf, sizeof(buf), "%4ld ", (long)size);
        else if (size < 1024*1024) snprintf(buf, sizeof(buf), "%4ldK", (long)(size/1024));
        else if (size < 1024LL*1024*1024) snprintf(buf, sizeof(buf), "%4ldM", (long)(size/(1024*1024)));
        else snprintf(buf, sizeof(buf), "%4ldG", (long)(size/(1024*1024*1024)));
        return buf;
}

static const char *
format_time(time_t mtime)
{
        static char buf[32];
        struct tm *tm = localtime(&mtime);
        if (!tm) return "?\?\?\?-?\?-?\? ?\?:?\?";
        // Show year if older than 6 months, otherwise show time
        time_t now = time(NULL);
        if (now - mtime > 180*24*3600 || now < mtime) {
                strftime(buf, sizeof(buf), "%b %d  %Y", tm);
        } else {
                strftime(buf, sizeof(buf), "%b %d %H:%M", tm);
        }
        return buf;
}

static void
selection_up(fex_context *ctx)
{
        if (ctx->entries.i > 0) {
                --ctx->entries.i;
        }
}

static void
selection_down(fex_context *ctx)
{
        if (ctx->entries.i < ctx->entries.fes.len-1) {
                ++ctx->entries.i;
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
                        char *path = ctx->entries.fes.data[*ar[i]].name;
                        if (!strcmp(path, "..") || !strcmp(path, ".")) {
                                continue;
                        }
                        dyn_array_append(confirm, path);
                        dyn_array_append(indices, *ar[i]);
                }
                free(ar);
        } else {
                char *path = ctx->entries.fes.data[ctx->entries.i].name;
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

        const char *path = ctx->entries.fes.data[ctx->entries.i].name;

        forge_ctrl_cursor_to_first_line();
        CURSOR_DOWN(ctx->entries.i + 1);
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
                for (size_t i = ctx->entries.i+1; i < ctx->entries.fes.len; ++i) {
                        if (forge_utils_regex(ctx->last_query, ctx->entries.fes.data[i].name)) {
                                ctx->entries.i = i;
                                break;
                        }
                }
        } else {
                for (size_t i = ctx->entries.i-1; i > 0; --i) {
                        if (forge_utils_regex(ctx->last_query, ctx->entries.fes.data[i].name)) {
                                ctx->entries.i = i;
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
        if (ctx->entries.i == 0) {
                for (size_t i = 2; i < ctx->entries.fes.len; ++i) {
                        if (!mark && sizet_set_contains(&ctx->marked, i)) {
                                sizet_set_remove(&ctx->marked, i);
                        } else if (mark && !sizet_set_contains(&ctx->marked, i)) {
                                sizet_set_insert(&ctx->marked, i);
                        }
                }
        } else if (ctx->entries.i != 1) {
                if (!mark && sizet_set_contains(&ctx->marked, ctx->entries.i)) {
                        sizet_set_remove(&ctx->marked, ctx->entries.i);
                } else if (mark && !sizet_set_contains(&ctx->marked, ctx->entries.i)) {
                        sizet_set_insert(&ctx->marked, ctx->entries.i);
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

        ctx->entries.i = 0;

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
                                char *fullpath = forge_io_resolve_absolute_path(files[i]);
                                FE fe = {0};
                                fe.name = files[i];
                                fe.owner = NULL;
                                fe.group = NULL;
                                fe.stat_failed = (lstat(fullpath, &fe.st) == -1);

                                if (!fe.stat_failed) {
                                        struct passwd *pw = getpwuid(fe.st.st_uid);
                                        struct group  *gr = getgrgid(fe.st.st_gid);
                                        fe.owner = pw ? strdup(pw->pw_name) : strdup("?");
                                        fe.group = gr ? strdup(gr->gr_name) : strdup("?");
                                } else {
                                        fe.owner = strdup("?");
                                        fe.group = strdup("?");
                                        memset(&fe.st, 0, sizeof(fe.st));
                                }

                                dyn_array_append(ctx->entries.fes, fe);
                        }
                }

                // If we are out-of-bounds (from deleting, marking, etc.) move
                // to valid location.
                while (ctx->entries.i > ctx->entries.fes.len-1) {
                        --ctx->entries.i;
                }

                // Header
                char *abspath = forge_io_resolve_absolute_path(ctx->filepath);
                printf("Directory listing for " INVERT BLUE "%s" RESET "\n", abspath);
                free(abspath);

                // Print files
                size_t dirs_n = 0;
                size_t start = ctx->hoffset;
                size_t end = start + ctx->term.h - 2;
                if (end > ctx->entries.fes.len)
                        end = ctx->entries.fes.len;
                for (size_t i = start; i < end; ++i) {
                        FE *e = &ctx->entries.fes.data[i];
                        int is_selected = (i == ctx->entries.i);
                        int is_marked   = sizet_set_contains(&ctx->marked, i);
                        int is_dir      = !e->stat_failed && S_ISDIR(e->st.st_mode);

                        if (!strcmp(e->name, "..") || !strcmp(e->name, ".")) {
                                printf(GRAY);
                                ++dirs_n;
                        }
                        else if (is_dir) {
                                printf(BOLD CYAN);
                                ++dirs_n;
                        } else if (!e->stat_failed && (e->st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH))) {
                                printf(GREEN);  // executable
                        } else {
                                printf(WHITE);
                        }

                        if (is_selected) printf(INVERT);
                        if (is_marked)   printf(PINK "<M> ");

                        char modebuf[11] = "??????????";
                        if (!e->stat_failed) mode_string(e->st.st_mode, modebuf);

                        const char *size_str = e->stat_failed ? "     ? " : human_size(e->st.st_size);
                        const char *time_str = e->stat_failed ? "?????????????" : format_time(e->st.st_mtime);

                        printf("%s %3ld %-8s %-8s %s %s %s",
                               modebuf,
                               e->stat_failed ? 0L : (long)e->st.st_nlink,
                               e->owner ? e->owner : "?",
                               e->group ? e->group : "?",
                               size_str,
                               time_str,
                               e->name);

                        // Symlink target
                        if (!e->stat_failed && S_ISLNK(e->st.st_mode)) {
                                char fullpath[PATH_MAX];
                                snprintf(fullpath, sizeof(fullpath), "%s/%s", ctx->filepath, e->name);
                                char target[PATH_MAX];
                                ssize_t len = readlink(fullpath, target, sizeof(target)-1);
                                if (len != -1) {
                                        target[len] = '\0';
                                        printf(" -> " CYAN "%s" RESET, target);
                                }
                        }

                        // Show ghosted full path on selected line
                        if (is_selected) {
                                char fullpath[PATH_MAX];
                                snprintf(fullpath, sizeof(fullpath), "%s/%s", ctx->filepath, e->name);
                                char *abs = forge_io_resolve_absolute_path(fullpath);
                                printf(RESET "  " ITALIC GRAY "%s" RESET, abs);
                                free(abs);
                        }

                        putchar('\n');
                        printf(RESET);
                }

                // Directory status
                printf(BOLD WHITE "%zu items" RESET "  (%zu dirs)" RESET "  [" YELLOW "%zu" RESET "/" YELLOW "%zu" RESET "]\n",
                       ctx->entries.fes.len - 2,
                       dirs_n - 2,
                       ctx->entries.i+1,
                       ctx->entries.fes.len);


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
                                if (cd_selection(ctx, ctx->entries.fes.data[ctx->entries.i].name)) {
                                        ctx->entries.i = 0;
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
                if (ctx->entries.i >= ctx->hoffset + visible_lines) {
                        ctx->hoffset = ctx->entries.i - visible_lines + 1;
                }

                // Scroll up when selection reaches top of screen
                if (ctx->entries.i < ctx->hoffset) {
                        ctx->hoffset = ctx->entries.i;
                }

                // Clamp hoffset to valid range
                if (ctx->hoffset + visible_lines > ctx->entries.fes.len) {
                        ctx->hoffset = ctx->entries.fes.len > visible_lines ?
                                ctx->entries.fes.len - visible_lines : 0;
                }
                if (ctx->hoffset >= ctx->entries.fes.len) {
                        ctx->hoffset = 0;
                }

                if (fs_changed) {
                        for (size_t i = 0; files[i]; ++i) {
                                free(files[i]);
                                free(ctx->entries.fes.data[i].owner);
                                free(ctx->entries.fes.data[i].group);
                        }
                        free(files);
                        dyn_array_clear(ctx->entries.fes);
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
                .entries = {
                        .i = 0,
                        .fes = dyn_array_empty(FE_array),
                },
                .filepath = filepath,
                .marked = sizet_set_create(sizet_hash, sizet_cmp, NULL),
                .last_query = NULL,
                .hoffset = 0,
        };

        display(&ctx);

        return 0;
}
