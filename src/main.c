#define _GNU_SOURCE  /* getline, strdup, setenv, sync, prctl, reboot */

/***************************************************************************//**

  @file         main.c

  @author       Stephen Brennan (original lsh, 2015)
                jvmlab contributors (PID 1, hardening, tokenizer, script
                mode, poweroff/reboot builtins).

  @origin       https://github.com/brenns10/lsh  (tutorial, public domain).
                Tutorial write-up: http://brennan.io/2015/01/16/write-a-shell-in-c/

  @brief        jvmlab-lsh: Stephen Brennan's teaching shell, adapted to
                run as /bin/sh and PID 1 on the jvmlab minimal Linux
                image. The lsh_loop / lsh_execute / builtin-table
                skeleton is upstream; everything about init duties
                (signal handling, orphan reaping, prctl name, poweroff
                on exit) plus the tokenizer and script / -c modes is
                jvmlab. See ../README.md for the divergence summary.

*******************************************************************************/

#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/prctl.h>
#include <sys/reboot.h>

/* PID 1 flag set once in main(); guards every exit path so the kernel
   never panics with "attempted to kill init". */
static int is_init = 0;

/* Exit status of the last command, exposed to the tokenizer as $?. */
static int last_status = 0;

int lsh_cd(char **args);
int lsh_help(char **args);
int lsh_exit(char **args);
int lsh_true(char **args);
int lsh_false(char **args);
int lsh_colon(char **args);
int lsh_poweroff(char **args);
int lsh_reboot(char **args);

char *builtin_str[] = {
  "cd",
  "help",
  "exit",
  "true",
  "false",
  ":",
  "poweroff",
  "halt",
  "reboot"
};

int (*builtin_func[]) (char **) = {
  &lsh_cd,
  &lsh_help,
  &lsh_exit,
  &lsh_true,
  &lsh_false,
  &lsh_colon,
  &lsh_poweroff,
  &lsh_poweroff,   /* halt is an alias */
  &lsh_reboot
};

int lsh_num_builtins(void) {
  return sizeof(builtin_str) / sizeof(char *);
}

/*
  Builtin function implementations.
*/

int lsh_cd(char **args)
{
  if (args[1] == NULL) {
    fprintf(stderr, "lsh: expected argument to \"cd\"\n");
    last_status = 1;
  } else if (chdir(args[1]) != 0) {
    perror("lsh");
    last_status = 1;
  } else {
    last_status = 0;
  }
  return 1;
}

int lsh_help(char **args)
{
  (void)args;
  int i;
  printf("Stephen Brennan's LSH (jvmlab MVP)\n");
  printf("Type program names and arguments, and hit enter.\n");
  printf("The following are built in:\n");

  for (i = 0; i < lsh_num_builtins(); i++) {
    printf("  %s\n", builtin_str[i]);
  }

  printf("Use the man command for information on other programs.\n");
  last_status = 0;
  return 1;
}

int lsh_exit(char **args)
{
  (void)args;
  /* As PID 1, `exit` triggers a controlled poweroff (the only way to
     "leave" init that doesn't panic the kernel). Otherwise terminate
     the shell normally. */
  if (is_init) {
    return lsh_poweroff(args);
  }
  last_status = 0;
  return 0;
}

/* POSIX `true`, `false`, and `:` as builtins so scripts and `echo $?`
   work even when /bin has no standalone binaries for them. */
int lsh_true(char **args)  { (void)args; last_status = 0; return 1; }
int lsh_false(char **args) { (void)args; last_status = 1; return 1; }
int lsh_colon(char **args) { (void)args; last_status = 0; return 1; }

/* Power-control builtins. Require CAP_SYS_BOOT (PID 1 in this image
   runs as root, so they just work). */
int lsh_poweroff(char **args)
{
  (void)args;
  sync();
  reboot(RB_POWER_OFF);
  perror("lsh: poweroff");
  last_status = 1;
  return 1;
}

int lsh_reboot(char **args)
{
  (void)args;
  sync();
  reboot(RB_AUTOBOOT);
  perror("lsh: reboot");
  last_status = 1;
  return 1;
}

/**
  @brief Launch a program and wait for it to terminate.
 */
int lsh_launch(char **args)
{
  pid_t pid;
  int status;

  pid = fork();
  if (pid == 0) {
    /* Child: restore default signal handling before exec so Ctrl-C, etc.
       behave normally inside the spawned program. */
    signal(SIGINT,  SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    if (execvp(args[0], args) == -1) {
      perror("lsh");
    }
    _exit(127);
  } else if (pid < 0) {
    perror("lsh");
    last_status = 1;
  } else {
    do {
      if (waitpid(pid, &status, WUNTRACED) == -1) {
        if (errno == EINTR) continue;
        perror("lsh");
        last_status = 1;
        return 1;
      }
    } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    last_status = WIFEXITED(status) ? WEXITSTATUS(status)
                                    : 128 + WTERMSIG(status);
  }
  return 1;
}

/**
  @brief Execute shell built-in or launch program.
 */
int lsh_execute(char **args)
{
  int i;

  if (args == NULL || args[0] == NULL) {
    return 1;
  }

  for (i = 0; i < lsh_num_builtins(); i++) {
    if (strcmp(args[0], builtin_str[i]) == 0) {
      return (*builtin_func[i])(args);
    }
  }

  return lsh_launch(args);
}

/**
  @brief Read a line from a stream. Returns heap-allocated string, or NULL
         on EOF / unrecoverable error. PID 1 never returns NULL in
         interactive mode; it returns an empty heap string so the caller
         loops.
 */
char *lsh_read_line(FILE *in, int interactive)
{
  char *line = NULL;
  size_t bufsize = 0;
  ssize_t n;

  errno = 0;
  n = getline(&line, &bufsize, in);
  if (n == -1) {
    free(line);
    if (feof(in)) {
      if (is_init && interactive) {
        /* Block briefly and keep the prompt loop alive. */
        clearerr(in);
        sleep(1);
        return strdup("");
      }
      return NULL;
    }
    perror("lsh: getline");
    if (is_init && interactive) {
      clearerr(in);
      sleep(1);
      return strdup("");
    }
    return NULL;
  }
  return line;
}

/*
  Tokenizer: quoting ('...', "..." with \-escapes), backslash escapes,
  '#' comments to end of line, $? expansion. Allocates fresh storage per
  token; caller frees with lsh_free_tokens().
*/

static int push_char(char **buf, size_t *len, size_t *cap, char c)
{
  if (*len + 1 >= *cap) {
    size_t nc = *cap ? *cap * 2 : 32;
    char *nb = realloc(*buf, nc);
    if (!nb) return -1;
    *buf = nb;
    *cap = nc;
  }
  (*buf)[(*len)++] = c;
  return 0;
}

static int push_token(char ***toks, size_t *count, size_t *cap, char *tok)
{
  if (*count + 2 > *cap) {
    size_t nc = *cap ? *cap * 2 : 8;
    char **nt = realloc(*toks, nc * sizeof(char *));
    if (!nt) return -1;
    *toks = nt;
    *cap = nc;
  }
  (*toks)[(*count)++] = tok;
  (*toks)[*count] = NULL;
  return 0;
}

void lsh_free_tokens(char **tokens)
{
  if (!tokens) return;
  for (size_t i = 0; tokens[i]; i++) free(tokens[i]);
  free(tokens);
}

static int push_status_digits(char **cur, size_t *clen, size_t *ccap)
{
  char numbuf[16];
  int nl = snprintf(numbuf, sizeof(numbuf), "%d", last_status);
  for (int i = 0; i < nl; i++) {
    if (push_char(cur, clen, ccap, numbuf[i]) == -1) return -1;
  }
  return 0;
}

char **lsh_tokenize(const char *line)
{
  char **tokens = calloc(1, sizeof(char *));
  size_t tcount = 0, tcap = 1;
  char *cur = NULL;
  size_t clen = 0, ccap = 0;
  int in_token = 0;
  const char *p;

  if (!tokens) goto oom;

  for (p = line; *p; ) {
    char c = *p;

    if (!in_token && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
      p++;
      continue;
    }

    if (!in_token && c == '#') {
      break;
    }

    in_token = 1;

    if (c == '\'') {
      p++;
      while (*p && *p != '\'') {
        if (push_char(&cur, &clen, &ccap, *p) == -1) goto oom;
        p++;
      }
      if (*p == '\'') p++;
      continue;
    }

    if (c == '"') {
      p++;
      while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
          char esc = p[1];
          char out;
          switch (esc) {
            case 'n':  out = '\n'; break;
            case 't':  out = '\t'; break;
            case '\\': out = '\\'; break;
            case '"':  out = '"';  break;
            case '$':  out = '$';  break;
            default:   out = esc;  break;
          }
          if (push_char(&cur, &clen, &ccap, out) == -1) goto oom;
          p += 2;
        } else if (*p == '$' && p[1] == '?') {
          if (push_status_digits(&cur, &clen, &ccap) == -1) goto oom;
          p += 2;
        } else {
          if (push_char(&cur, &clen, &ccap, *p) == -1) goto oom;
          p++;
        }
      }
      if (*p == '"') p++;
      continue;
    }

    if (c == '\\' && p[1]) {
      if (push_char(&cur, &clen, &ccap, p[1]) == -1) goto oom;
      p += 2;
      continue;
    }

    if (c == '$' && p[1] == '?') {
      if (push_status_digits(&cur, &clen, &ccap) == -1) goto oom;
      p += 2;
      continue;
    }

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (push_char(&cur, &clen, &ccap, '\0') == -1) goto oom;
      if (push_token(&tokens, &tcount, &tcap, cur) == -1) goto oom;
      cur = NULL; clen = 0; ccap = 0;
      in_token = 0;
      p++;
      continue;
    }

    if (push_char(&cur, &clen, &ccap, c) == -1) goto oom;
    p++;
  }

  if (in_token) {
    if (push_char(&cur, &clen, &ccap, '\0') == -1) goto oom;
    if (push_token(&tokens, &tcount, &tcap, cur) == -1) goto oom;
    cur = NULL;
  }

  return tokens;

oom:
  free(cur);
  lsh_free_tokens(tokens);
  fprintf(stderr, "lsh: allocation error\n");
  return NULL;
}

/**
  @brief Main read/execute loop. Reads from `in`; only prompts when
         `interactive` is non-zero.
 */
void lsh_run(FILE *in, int interactive)
{
  char *line;
  char **args;
  int status = 1;

  do {
    /* Opportunistically reap any orphaned children (matters when lsh is
       PID 1 and a grandchild outlives its parent). */
    while (waitpid(-1, NULL, WNOHANG) > 0) { }

    if (interactive) {
      printf("> ");
      fflush(stdout);
    }
    line = lsh_read_line(in, interactive);
    if (!line) break;
    args = lsh_tokenize(line);
    free(line);
    if (!args) {
      if (is_init) continue;
      break;
    }
    status = lsh_execute(args);
    lsh_free_tokens(args);
  } while (status);
}

/**
  @brief Main entry point.
         Usage:
           lsh              interactive
           lsh -c "cmd"     run a single command line
           lsh script.sh    run a script file
 */
int main(int argc, char **argv)
{
  is_init = (getpid() == 1);

  /* /proc/<pid>/comm is set by the kernel from basename(bprm->filename),
     not from argv[0]. When the kernel exec'd us via /bin/sh (binfmt_script
     for /init, or our own safety-net), comm would otherwise read "sh".
     Force it to "lsh" so process listings are unambiguous. */
  prctl(PR_SET_NAME, (unsigned long)"lsh", 0, 0, 0);

  setenv("PATH", "/bin", 0);

  /* Keep SIGCHLD at its default: setting it to SIG_IGN auto-reaps on
     Linux but then makes waitpid() for our own child fail with ECHILD.
     We explicitly waitpid() in lsh_launch and reap orphans opportunistically
     at the top of the read loop. */
  signal(SIGCHLD, SIG_DFL);
  /* Ignore interactive signals in the shell itself; children reset to
     SIG_DFL before exec. */
  signal(SIGINT,  SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGTSTP, SIG_IGN);

  if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
    char **args = lsh_tokenize(argv[2]);
    if (args) {
      lsh_execute(args);
      lsh_free_tokens(args);
    }
  } else if (argc >= 2) {
    FILE *f = fopen(argv[1], "r");
    if (!f) {
      perror("lsh");
      return 1;
    }
    lsh_run(f, 0);
    fclose(f);
  } else {
    lsh_run(stdin, isatty(fileno(stdin)));
  }

  if (is_init) {
    /* Safety net: PID 1 must not return to the kernel. Drop to an
       interactive shell; if that fails for any reason, wait forever. */
    execl("/bin/lsh", "lsh", (char *)NULL);
    for (;;) pause();
  }
  return last_status;
}
