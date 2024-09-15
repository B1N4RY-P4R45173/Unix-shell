/*
 * mysh - a small Unix shell.
 *
 * Supports pipelines (a | b | c), I/O redirection (<, >, >>, 2>),
 * background jobs (&), command lists (;), quoting, and job control
 * (Ctrl-C, Ctrl-Z, jobs, fg, bg).
 */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define PROMPT     "mysh$ "
#define MAX_TOKENS 256
#define MAX_ARGS   64
#define MAX_PROCS  16
#define MAX_JOBS   32
#define CMD_LEN    256

/* ---------- data structures ---------- */

typedef enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APPEND, T_ERR, T_AMP, T_SEMI } tok_type;

typedef struct {
    tok_type type;
    char *text;                 /* only set for T_WORD */
} token;

typedef struct {
    char *argv[MAX_ARGS + 1];
    int argc;
    char *in, *out, *err;       /* redirection targets, or NULL */
    int append;                 /* out was given with >> */
} command;

typedef struct {
    command cmds[MAX_PROCS];
    int ncmds;
    int background;
    char text[CMD_LEN];         /* command line, for job listings */
} pipeline;

typedef enum { P_RUNNING, P_STOPPED, P_DONE } pstate;

typedef struct {
    pid_t pid;
    pstate state;
    int status;                 /* raw waitpid status once P_DONE */
} proc;

typedef struct {
    int used;
    int id;
    pid_t pgid;
    proc procs[MAX_PROCS];
    int nprocs;
    int background;
    int notified;               /* user has been told about the current state */
    unsigned long seq;          /* larger = more recent; picks the "current" job */
    struct termios tmodes;      /* terminal modes saved when the job stopped */
    int has_tmodes;
    char text[CMD_LEN];
} job;

static job jobs[MAX_JOBS];
static unsigned long job_seq;
static int interactive;
static int shell_terminal = STDIN_FILENO;
static pid_t shell_pgid;
static struct termios shell_tmodes;
static int last_status;

/* ---------- job table ---------- */

static job *job_alloc(void)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) {
            memset(&jobs[i], 0, sizeof jobs[i]);
            jobs[i].used = 1;
            jobs[i].id = i + 1;
            jobs[i].seq = ++job_seq;
            return &jobs[i];
        }
    }
    return NULL;
}

static pstate job_state(const job *j)
{
    int stopped = 0;
    for (int i = 0; i < j->nprocs; i++) {
        if (j->procs[i].state == P_RUNNING)
            return P_RUNNING;
        if (j->procs[i].state == P_STOPPED)
            stopped = 1;
    }
    return stopped ? P_STOPPED : P_DONE;
}

/* Record a waitpid() result. Called from the SIGCHLD handler, so it only touches memory. */
static void mark_status(pid_t pid, int status)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used)
            continue;
        for (int k = 0; k < jobs[i].nprocs; k++) {
            proc *p = &jobs[i].procs[k];
            if (p->pid != pid)
                continue;
            if (WIFSTOPPED(status)) {
                p->state = P_STOPPED;
            } else if (WIFCONTINUED(status)) {
                p->state = P_RUNNING;
            } else {
                p->state = P_DONE;
                p->status = status;
            }
            jobs[i].notified = 0;
            return;
        }
    }
}

/* The job fg/bg act on by default: the most recently started or stopped one. */
static job *current_job(void)
{
    job *best = NULL;
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used && job_state(&jobs[i]) != P_DONE && (!best || jobs[i].seq > best->seq))
            best = &jobs[i];
    return best;
}

static job *find_job(const char *spec)
{
    if (!spec)
        return current_job();
    if (*spec == '%')
        spec++;
    char *end;
    long id = strtol(spec, &end, 10);
    if (*spec == '\0' || *end != '\0' || id < 1 || id > MAX_JOBS || !jobs[id - 1].used)
        return NULL;
    return &jobs[id - 1];
}

static int status_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 0;
}

static void block_sigchld(sigset_t *prev)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, prev);
}

static void print_job(const job *j, const char *state)
{
    printf("[%d]%c  %-24s%s%s\n", j->id, j == current_job() ? '+' : ' ',
           state, j->text, j->background && job_state(j) == P_RUNNING ? " &" : "");
}

/* Report finished and newly stopped background jobs, then drop finished ones. */
static void notify_jobs(void)
{
    sigset_t prev;
    block_sigchld(&prev);
    for (int i = 0; i < MAX_JOBS; i++) {
        job *j = &jobs[i];
        if (!j->used)
            continue;
        pstate st = job_state(j);
        if (st == P_DONE) {
            int code = status_code(j->procs[j->nprocs - 1].status);
            char buf[32];
            if (code == 0)
                snprintf(buf, sizeof buf, "Done");
            else
                snprintf(buf, sizeof buf, "Exit %d", code);
            print_job(j, buf);
            j->used = 0;
        } else if (st == P_STOPPED && !j->notified) {
            j->seq = ++job_seq;
            print_job(j, "Stopped");
            j->notified = 1;
        }
    }
    fflush(stdout);
    sigprocmask(SIG_SETMASK, &prev, NULL);
}

/*
 * Give the terminal to job j and wait until it exits or stops.
 * The caller must have SIGCHLD blocked so the handler can't reap our processes.
 */
static int foreground_job(job *j, int cont)
{
    if (interactive) {
        tcsetpgrp(shell_terminal, j->pgid);
        if (cont && j->has_tmodes)
            tcsetattr(shell_terminal, TCSADRAIN, &j->tmodes);
    }
    if (cont)
        kill(-j->pgid, SIGCONT);

    while (job_state(j) == P_RUNNING) {
        int status;
        pid_t pid = waitpid(-j->pgid, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        mark_status(pid, status);
    }

    if (interactive) {
        tcsetpgrp(shell_terminal, shell_pgid);
        j->has_tmodes = tcgetattr(shell_terminal, &j->tmodes) == 0;
        tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
    }

    if (job_state(j) == P_STOPPED) {
        j->background = 1;
        j->notified = 1;
        j->seq = ++job_seq;
        printf("\n");
        print_job(j, "Stopped");
        return 128 + SIGTSTP;
    }

    int status = j->procs[j->nprocs - 1].status;
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGINT)
            printf("\n");
        else if (sig != SIGPIPE)
            printf("%s%s\n", strsignal(sig),
#ifdef WCOREDUMP
                   WCOREDUMP(status) ? " (core dumped)" :
#endif
                   "");
    }
    j->used = 0;
    return status_code(status);
}

/* Send SIGHUP to every job before the shell exits, like an interactive bash. */
static void hangup_jobs(void)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].used && job_state(&jobs[i]) != P_DONE) {
            kill(-jobs[i].pgid, SIGHUP);
            kill(-jobs[i].pgid, SIGCONT);
        }
    }
}

/* ---------- signals ---------- */

static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
        mark_status(pid, status);
    errno = saved_errno;
}

/* Ctrl-C at the prompt: the tty already discarded the line, just redraw. */
static void sigint_handler(int sig)
{
    (void)sig;
    static const char msg[] = "\n" PROMPT;
    int saved_errno = errno;
    ssize_t r = write(STDOUT_FILENO, msg, sizeof msg - 1);
    (void)r;
    errno = saved_errno;
}

static void init_shell(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    interactive = isatty(shell_terminal);
    if (!interactive)
        return;

    /* If we were started in the background, wait until we're in the foreground. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
        kill(-shell_pgid, SIGTTIN);

    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    /* Put the shell in its own process group and take the terminal. */
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EPERM) {
        perror("mysh: setpgid");
        exit(1);
    }
    tcsetpgrp(shell_terminal, shell_pgid);
    tcgetattr(shell_terminal, &shell_tmodes);
}

/* ---------- tokenizer ---------- */

static void free_tokens(token *toks, int n)
{
    for (int i = 0; i < n; i++)
        free(toks[i].text);
}

static void push_char(char **buf, size_t *len, size_t *cap, char c)
{
    if (*len + 1 >= *cap) {
        *cap *= 2;
        char *p = realloc(*buf, *cap);
        if (!p) {
            perror("mysh: realloc");
            exit(1);
        }
        *buf = p;
    }
    (*buf)[(*len)++] = c;
}

static int is_word_end(char c)
{
    return c == '\0' || strchr(" \t\n|<>&;", c) != NULL;
}

/* Split a line into tokens. Returns the count, or -1 on error (already reported). */
static int tokenize(const char *s, token *toks, int max)
{
    int n = 0;
    while (*s) {
        if (*s == ' ' || *s == '\t' || *s == '\n') {
            s++;
            continue;
        }
        if (*s == '#')
            break;
        if (n == max) {
            fprintf(stderr, "mysh: too many tokens\n");
            goto fail;
        }

        token *t = &toks[n];
        t->text = NULL;
        if (*s == '|') {
            t->type = T_PIPE;
            s++;
        } else if (*s == '<') {
            t->type = T_IN;
            s++;
        } else if (*s == '>' && s[1] == '>') {
            t->type = T_APPEND;
            s += 2;
        } else if (*s == '>') {
            t->type = T_OUT;
            s++;
        } else if (*s == '2' && s[1] == '>') {
            t->type = T_ERR;
            s += 2;
        } else if (*s == '&') {
            t->type = T_AMP;
            s++;
        } else if (*s == ';') {
            t->type = T_SEMI;
            s++;
        } else {
            size_t len = 0, cap = 64;
            char *buf = malloc(cap);
            if (!buf) {
                perror("mysh: malloc");
                exit(1);
            }
            /* Unquoted leading ~ expands to $HOME. */
            const char *home = getenv("HOME");
            if (*s == '~' && home && (s[1] == '/' || is_word_end(s[1]))) {
                for (const char *h = home; *h; h++)
                    push_char(&buf, &len, &cap, *h);
                s++;
            }
            while (!is_word_end(*s)) {
                if (*s == '\'') {
                    for (s++; *s && *s != '\''; s++)
                        push_char(&buf, &len, &cap, *s);
                } else if (*s == '"') {
                    for (s++; *s && *s != '"'; s++) {
                        if (*s == '\\' && (s[1] == '"' || s[1] == '\\' || s[1] == '$'))
                            s++;
                        push_char(&buf, &len, &cap, *s);
                    }
                } else if (*s == '\\' && s[1]) {
                    push_char(&buf, &len, &cap, s[1]);
                    s += 2;
                    continue;
                } else {
                    push_char(&buf, &len, &cap, *s++);
                    continue;
                }
                if (!*s) {
                    fprintf(stderr, "mysh: unterminated quote\n");
                    free(buf);
                    goto fail;
                }
                s++;            /* closing quote */
            }
            buf[len] = '\0';
            t->type = T_WORD;
            t->text = buf;
        }
        n++;
    }
    return n;

fail:
    free_tokens(toks, n);
    return -1;
}

/* ---------- parser ---------- */

static const char *tok_str(const token *t)
{
    switch (t->type) {
    case T_WORD:   return t->text;
    case T_PIPE:   return "|";
    case T_IN:     return "<";
    case T_OUT:    return ">";
    case T_APPEND: return ">>";
    case T_ERR:    return "2>";
    case T_AMP:    return "&";
    case T_SEMI:   return ";";
    }
    return "";
}

/* Parse toks[start, end) into a pipeline. argv and file names point into the tokens. */
static int parse_pipeline(token *toks, int start, int end, pipeline *p)
{
    memset(p, 0, sizeof *p);
    p->ncmds = 1;
    command *c = &p->cmds[0];

    for (int i = start; i < end; i++) {
        token *t = &toks[i];
        switch (t->type) {
        case T_WORD:
            if (c->argc == MAX_ARGS) {
                fprintf(stderr, "mysh: too many arguments\n");
                return -1;
            }
            c->argv[c->argc++] = t->text;
            break;
        case T_PIPE:
            if (c->argc == 0)
                goto syntax;
            if (p->ncmds == MAX_PROCS) {
                fprintf(stderr, "mysh: pipeline too long\n");
                return -1;
            }
            c = &p->cmds[p->ncmds++];
            break;
        case T_IN:
        case T_OUT:
        case T_APPEND:
        case T_ERR: {
            if (i + 1 >= end || toks[i + 1].type != T_WORD)
                goto syntax;
            char *file = toks[++i].text;
            if (t->type == T_IN) {
                c->in = file;
            } else if (t->type == T_ERR) {
                c->err = file;
            } else {
                c->out = file;
                c->append = t->type == T_APPEND;
            }
            break;
        }
        default:
            goto syntax;
        }
    }
    if (c->argc == 0)
        goto syntax;

    size_t off = 0;
    for (int i = start; i < end; i++) {
        int w = snprintf(p->text + off, sizeof p->text - off, "%s%s",
                         i > start ? " " : "", tok_str(&toks[i]));
        if (w < 0 || (size_t)w >= sizeof p->text - off)
            break;
        off += w;
    }
    return 0;

syntax:
    fprintf(stderr, "mysh: syntax error\n");
    return -1;
}

/* ---------- builtins ---------- */

static int is_builtin(const char *name)
{
    return !strcmp(name, "cd") || !strcmp(name, "exit") || !strcmp(name, "jobs") ||
           !strcmp(name, "fg") || !strcmp(name, "bg");
}

static int builtin_cd(command *c)
{
    const char *dir = c->argc > 1 ? c->argv[1] : getenv("HOME");
    if (!dir) {
        fprintf(stderr, "mysh: cd: HOME not set\n");
        return 1;
    }
    if (chdir(dir) < 0) {
        fprintf(stderr, "mysh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    return 0;
}

static int builtin_jobs(void)
{
    sigset_t prev;
    block_sigchld(&prev);
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used)
            continue;
        pstate st = job_state(&jobs[i]);
        print_job(&jobs[i], st == P_RUNNING ? "Running" : st == P_STOPPED ? "Stopped" : "Done");
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);
    return 0;
}

static int builtin_fg_bg(command *c, int to_background)
{
    sigset_t prev;
    block_sigchld(&prev);

    job *j = find_job(c->argc > 1 ? c->argv[1] : NULL);
    if (!j || job_state(j) == P_DONE) {
        fprintf(stderr, "mysh: %s: no such job\n", c->argv[0]);
        sigprocmask(SIG_SETMASK, &prev, NULL);
        return 1;
    }

    for (int i = 0; i < j->nprocs; i++)
        if (j->procs[i].state == P_STOPPED)
            j->procs[i].state = P_RUNNING;
    j->notified = 0;

    int rc = 0;
    if (to_background) {
        j->background = 1;
        printf("[%d]+ %s &\n", j->id, j->text);
        fflush(stdout);
        kill(-j->pgid, SIGCONT);
    } else {
        j->background = 0;
        printf("%s\n", j->text);
        fflush(stdout);
        rc = foreground_job(j, 1);
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);
    return rc;
}

static int run_builtin(command *c)
{
    const char *name = c->argv[0];
    if (!strcmp(name, "cd"))
        return builtin_cd(c);
    if (!strcmp(name, "jobs"))
        return builtin_jobs();
    if (!strcmp(name, "fg"))
        return builtin_fg_bg(c, 0);
    if (!strcmp(name, "bg"))
        return builtin_fg_bg(c, 1);
    if (!strcmp(name, "exit")) {
        int code = c->argc > 1 ? atoi(c->argv[1]) : last_status;
        hangup_jobs();
        fflush(stdout);
        exit(code);
    }
    return 1;
}

/* ---------- execution ---------- */

static int redirect(const char *file, int flags, int target)
{
    int fd = open(file, flags, 0644);
    if (fd < 0) {
        fprintf(stderr, "mysh: %s: %s\n", file, strerror(errno));
        return -1;
    }
    if (fd != target) {
        dup2(fd, target);
        close(fd);
    }
    return 0;
}

static int apply_redirs(const command *c)
{
    if (c->in && redirect(c->in, O_RDONLY, STDIN_FILENO) < 0)
        return -1;
    if (c->out && redirect(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC),
                           STDOUT_FILENO) < 0)
        return -1;
    if (c->err && redirect(c->err, O_WRONLY | O_CREAT | O_TRUNC, STDERR_FILENO) < 0)
        return -1;
    return 0;
}

/* Run a builtin inside the shell process, with redirections undone afterwards. */
static int run_builtin_here(command *c)
{
    int saved[3] = { dup(STDIN_FILENO), dup(STDOUT_FILENO), dup(STDERR_FILENO) };
    int rc = 1;
    if (apply_redirs(c) == 0)
        rc = run_builtin(c);
    fflush(stdout);
    fflush(stderr);
    for (int fd = 0; fd < 3; fd++) {
        dup2(saved[fd], fd);
        close(saved[fd]);
    }
    return rc;
}

/* Runs in the forked child before exec: join the job's group and restore default signals. */
static void child_setup(pid_t pgid, int background)
{
    setpgid(0, pgid);
    if (interactive && !background)
        tcsetpgrp(shell_terminal, getpgrp());
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

static void run_pipeline(pipeline *p)
{
    command *first = &p->cmds[0];
    if (p->ncmds == 1 && !p->background && is_builtin(first->argv[0])) {
        last_status = run_builtin_here(first);
        return;
    }

    /* Keep SIGCHLD blocked until the job is fully registered (or finished). */
    sigset_t prev;
    block_sigchld(&prev);

    job *j = job_alloc();
    if (!j) {
        fprintf(stderr, "mysh: too many jobs\n");
        last_status = 1;
        sigprocmask(SIG_SETMASK, &prev, NULL);
        return;
    }
    j->background = p->background;
    memcpy(j->text, p->text, sizeof j->text);

    fflush(NULL);               /* don't let children inherit unflushed output */
    int in_fd = -1;
    for (int i = 0; i < p->ncmds; i++) {
        command *c = &p->cmds[i];
        int pfd[2] = { -1, -1 };
        if (i < p->ncmds - 1 && pipe(pfd) < 0) {
            perror("mysh: pipe");
            break;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("mysh: fork");
            if (pfd[0] != -1) {
                close(pfd[0]);
                close(pfd[1]);
            }
            break;
        }

        if (pid == 0) {
            child_setup(j->pgid, p->background);
            sigprocmask(SIG_SETMASK, &prev, NULL);
            if (in_fd != -1) {
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            }
            if (pfd[1] != -1) {
                dup2(pfd[1], STDOUT_FILENO);
                close(pfd[1]);
                close(pfd[0]);
            }
            if (apply_redirs(c) < 0)
                _exit(1);
            if (is_builtin(c->argv[0]))
                exit(run_builtin(c));
            execvp(c->argv[0], c->argv);
            fprintf(stderr, "mysh: %s: %s\n", c->argv[0],
                    errno == ENOENT ? "command not found" : strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }

        /* Set the group in the parent too, so it's in place no matter who runs first. */
        if (j->pgid == 0)
            j->pgid = pid;
        setpgid(pid, j->pgid);
        j->procs[j->nprocs++] = (proc){ pid, P_RUNNING, 0 };

        if (in_fd != -1)
            close(in_fd);
        if (pfd[1] != -1)
            close(pfd[1]);
        in_fd = pfd[0];
    }
    if (in_fd != -1)
        close(in_fd);

    if (j->nprocs == 0) {
        j->used = 0;
        last_status = 1;
    } else if (p->background) {
        printf("[%d] %d\n", j->id, (int)j->pgid);
        last_status = 0;
    } else {
        last_status = foreground_job(j, 0);
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);
}

/* A line is a list of pipelines separated by ';' or '&'. */
static void run_line(const char *line)
{
    token toks[MAX_TOKENS];
    int n = tokenize(line, toks, MAX_TOKENS);
    if (n <= 0)
        return;

    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i < n && toks[i].type != T_AMP && toks[i].type != T_SEMI)
            continue;
        if (i == start) {
            if (i < n)
                fprintf(stderr, "mysh: syntax error near '%s'\n", tok_str(&toks[i]));
            break;
        }
        pipeline p;
        if (parse_pipeline(toks, start, i, &p) < 0)
            break;
        p.background = i < n && toks[i].type == T_AMP;
        run_pipeline(&p);
        start = i + 1;
    }
    free_tokens(toks, n);
}

int main(void)
{
    init_shell();

    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        notify_jobs();
        if (interactive) {
            fputs(PROMPT, stdout);
            fflush(stdout);
        }
        errno = 0;
        if (getline(&line, &cap, stdin) < 0) {
            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            break;
        }
        run_line(line);
    }

    if (interactive)
        putchar('\n');
    hangup_jobs();
    free(line);
    return last_status;
}
