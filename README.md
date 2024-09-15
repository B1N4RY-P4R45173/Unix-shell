# mysh: a Unix shell in C

A small job-control shell written against POSIX system calls (`fork`, `execvp`,
`waitpid`, `pipe`, `dup2`, `setpgid`, `tcsetpgrp`, `sigaction`).

## Features

| Feature | Example |
|---|---|
| Pipelines of any length | `cat log | grep ERROR | sort | uniq -c` |
| I/O redirection | `sort < in > out`, `echo hi >> log`, `make 2> errors` |
| Background jobs | `sleep 30 &` prints `[1] 4242`, later `[1]+ Done  sleep 30` |
| Job control | Ctrl-Z, Ctrl-C, `jobs`, `fg %1`, `bg %1` |
| Command lists | `cd src; make & echo started` |
| Quoting | `'single'`, `"double \"escaped\""`, `back\ slash`, `~/path` |
| Builtins | `cd`, `exit [n]`, `jobs`, `fg`, `bg` |

## Build and run

Needs Linux, macOS or WSL. Native Windows has no `fork`.

```sh
make
./mysh
make test      # script tests + pty-driven job-control tests (needs python3)
```

## How it works

- **Pipelines**: the shell creates a `pipe()` between each pair of adjacent
  commands. Each child `dup2`s its ends onto stdin/stdout and closes every
  other pipe fd, so EOF and `SIGPIPE` propagate correctly (`yes | head` exits).
- **Process groups**: every pipeline runs as one job in its own process group.
  `setpgid` is called in both the parent and the child to avoid a race. A
  foreground job gets the terminal through `tcsetpgrp`, so the tty driver sends
  Ctrl-C/Ctrl-Z to the job and not to the shell.
- **Signals**: the shell ignores `SIGTSTP`/`SIGTTOU`/`SIGTTIN`/`SIGQUIT`, and its
  `SIGINT` handler only redraws the prompt. Children reset all of these to the
  defaults before `execvp`.
- **Reaping**: a `SIGCHLD` handler calls `waitpid(-1, WNOHANG | WUNTRACED | WCONTINUED)`
  and records each state change in the job table. `SIGCHLD` stays blocked while
  a job is being created or waited on in the foreground, so the handler can
  never reap a process the shell is waiting for. Finished background jobs are
  reported before the next prompt.
- **Terminal modes**: the shell saves the tty settings when a job stops and
  restores them on `fg`, so `vim`, then Ctrl-Z, then `fg` works.

## Not implemented

Variable expansion (`$HOME`, `$?`), globbing (`*.c`), `&&`/`||`, subshells,
here-docs and scripting constructs (`if`, `for`).
