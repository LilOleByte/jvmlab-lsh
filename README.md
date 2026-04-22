jvmlab-lsh
==========

A minimal C shell that runs as **PID 1** in the [jvmlab][jvmlab] minimal
Linux ISO, shipped as `/bin/sh`. Small enough to read end to end in an
afternoon, hardened enough to be the only userspace the appliance
needs.

## Where this code came from

This repository is a fork of **Stephen Brennan's `lsh`** — the
teaching shell from his 2015 article *["Write a Shell in C"][1]*. The
original is a deliberate teaching tool: a few hundred lines of C that
demonstrate the read–parse–fork–exec–wait loop without any of the
POSIX surface area a real shell needs.

Upstream: <https://github.com/brenns10/lsh>
Original licence: public domain (Unlicense) — see the upstream repo.

We kept Stephen's core structure — `lsh_loop`, `lsh_read_line`,
`lsh_execute`, the builtin table — because it is the clearest
expression of "what a shell is" that we know of. Every jvmlab change
sits on top of that skeleton; nothing was rewritten for its own sake.

## Why it is called jvmlab-lsh, not lsh

The fork point was the moment the shell became the init process
instead of a REPL. A PID 1 shell is a meaningfully different program,
with requirements that are orthogonal to Stephen's teaching goals, so
the fork gets its own name to avoid confusion with the upstream. The
concrete divergence:

- **It runs as PID 1.** Detects `getpid() == 1`, sets its own process
  name via `prctl(PR_SET_NAME)`, seeds `PATH=/bin`, installs signal
  disposition appropriate for init (`SIGINT` / `SIGQUIT` / `SIGTSTP`
  ignored, `SIGCHLD` left defaulted so `waitpid` works), and reaps
  orphaned children opportunistically from the main loop.
- **It does not exit.** When PID 1 the `exit` builtin triggers a
  clean `sync()` + `reboot(RB_POWER_OFF)`. If the main loop ever
  unwinds anyway, a safety net calls `execl("/bin/lsh", "lsh", NULL)`
  and then `pause()`s forever, so the kernel never panics with
  "attempted to kill init".
- **New builtins for appliance duties:** `true`, `false`, `:`,
  `poweroff`, `reboot`, `halt`. These exist because the ISO does not
  ship `/bin/true` or `/sbin/poweroff`; the shell has to be
  self-sufficient.
- **A real tokenizer.** `lsh_split_line` (the upstream's
  whitespace-only splitter) is replaced with a hand-written tokenizer
  supporting single- and double-quoted strings, backslash escapes
  inside double quotes, `#` end-of-line comments, and `$?` expansion
  so appliance init scripts can branch on the last exit status.
- **Script mode and `-c`.** `lsh /path/to/script` runs a file;
  `lsh -c 'command'` runs an inline command and exits with its
  status. Both reuse the same tokenizer and execution path as the
  interactive mode.
- **Hardened toolchain.** `make` with a modern GCC / clang builds a
  static PIE with Intel CET, stack canaries + guard-page probes, full
  RELRO, zero-initialised stack locals, non-executable stack, and
  format-string injection refused at compile time. See below and the
  [`Makefile`](Makefile) for flag-by-flag rationale.

The [`src/main.c`](src/main.c) file identifies upstream code paths
versus jvmlab additions in its header comment.

## Running outside jvmlab

You can still use this the way Stephen's tutorial intends — as a
standalone shell on a normal Linux host:

```sh
cc -o lsh src/main.c
./lsh
```

The PID 1 code paths are no-ops when `getpid()` is not 1, so running
it under your normal terminal is safe. The tokenizer, `-c`, and
script mode work everywhere.

Note: this fork uses `getline(3)`, `prctl(2)`, and `reboot(2)`, so
`#define _GNU_SOURCE` is present in `src/main.c`. The upstream's
`-DLSH_USE_STD_GETLINE` toggle is no longer relevant — we always use
the standard-library reader.

## Building for jvmlab

`make` (with no arguments) produces a hardened static PIE suitable
for shipping as `/bin/sh` / PID 1 in the minimal ISO. The toolchain
defaults to `musl-gcc` and the build assumes GCC >= 12 or clang >= 16
(ubuntu-24.04 and rolling-release distros like CachyOS both qualify).

Compile-time flags and why each is there:

- `-fstack-protector-strong` — canaries on any function with a local
  buffer or pointer-typed local.
- `-fstack-clash-protection` — guard-page probes on large stack
  allocations so a VLA / `alloca` cannot jump the guard into
  adjacent VM.
- `-fcf-protection=full` — Intel CET: `endbr64` at indirect-call
  targets (IBT) plus shadow-stack annotations. Harmless NOPs on
  non-CET CPUs; enforced in hardware on Tiger Lake / Alder Lake and
  newer.
- `-ftrivial-auto-var-init=zero` — zero every uninitialised stack
  variable on entry. Pairs with the kernel's
  `CONFIG_INIT_STACK_ALL_ZERO=y`.
- `-Wformat=2 -Werror=format-security` — user-controlled format
  strings refuse to compile.
- `-D_FORTIFY_SOURCE=2` — compile-time bounds checks on mem/str*.
- `-fPIE` — required for the link step below.

Link-time flags:

- `-static-pie` — static binary *and* relocatable, so the kernel
  applies ASLR on every exec. This is the single biggest win for
  ROP/JOP resistance in a statically-linked image.
- `-Wl,-z,noexecstack` — `GNU_STACK` is non-executable.
- `-Wl,-z,relro -Wl,-z,now` — full RELRO + immediate bind: every
  relocation is resolved at load time and the relocatable pages are
  mapped read-only afterwards, so an arbitrary write cannot be
  escalated via GOT overwrite.
- `-Wl,--gc-sections` + `-ffunction-sections -fdata-sections` —
  drops unreferenced functions/data so the shipped binary is
  exactly the code we use.

Override `CC`, `CFLAGS`, or `LDFLAGS` from the environment if you
need a different toolchain. `make install DESTDIR=/some/path` drops
`/bin/lsh` and a `/bin/sh` symlink into `/some/path/bin`.

Run `make smoke` from the repo root for the host-side test suite; it
resolves the binary by absolute path so it does not depend on the cwd
or on `tests/smoke.sh` keeping its executable bit through a shallow
clone.

## Contributing

Upstream `lsh` is a finished teaching artefact and does not accept
non-bug pull requests. This fork is different — jvmlab-lsh is a
working appliance component and welcomes changes that fit the threat
model documented in
[`jvmlab-build/THREAT_MODEL.md`](https://github.com/LilOleByte/jvmlab-build/blob/main/THREAT_MODEL.md).
Before proposing a new builtin or syntactic feature, check whether it
actually needs to live inside PID 1, or whether it belongs in
[`jvmlab-toybox`][toybox] instead.

## Licence

jvmlab-lsh ships under the [BSD Zero Clause License (0BSD)](LICENSE)
— the same licence as [`jvmlab-build`][jvmlab-build] and
[`jvmlab-toybox`][toybox]. 0BSD is effectively as permissive as the
public-domain dedication Stephen Brennan used upstream: you may use,
modify, and redistribute this code for any purpose, with or without
attribution, with no warranty.

Upstream code paths were released under the Unlicense, which
explicitly permits relicensing derivatives; `LICENSE` calls this out
and keeps Stephen's authorship credit intact. If you write about
jvmlab-lsh, please keep his attribution — he did the teaching work
that made this fork possible.

[jvmlab-build]: https://github.com/LilOleByte/jvmlab-build

[1]: http://brennan.io/2015/01/16/write-a-shell-in-c/
[jvmlab]: https://jvmlab.org/
[toybox]: https://github.com/LilOleByte/jvmlab-toybox
