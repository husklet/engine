# flinux's tty layer, and what Windows offers now

**Licensing.** flinux (Foreign Linux, Xiangyan Sun) is **GPLv3**. HL Engine is **MIT**. The two are
incompatible and no flinux code may enter `src/`. This document describes *techniques, algorithms, state
machines and constraints* — ideas, which are not copyrightable — and cites `file:line` so a reader can check
the claim against their tree. The few quoted fragments are short and are there to name a behaviour, not to
transliterate it. Any implementation that follows must be written independently from the understanding
recorded here, not from their source.

Every Windows claim below is one of three things, and they are never blurred: **measured** on this box
(Windows 11 Pro 10.0.26200.8457, clang 22.1.8 `x86_64-w64-windows-gnu`, programs kept in the scratchpad),
**cited** to flinux at `file:line` (commit `a041253`, 2016-03-29), or **explicitly marked reasoned-but-unverified**.
Where something was not measured, it says so.

`DOCS.md` is normative. This file is evidence and a recommendation.

---

## 1. Status

| | |
|---|---|
| Question asked | what does a guest see when termios is absent, and is that "considerably more than two weeks" |
| flinux's answer | a hand-written xterm emulator over the Win32 console — **1727 lines, 91 commits, 14 months** |
| Does ConPTY change it | **partly, and not the part people expect.** Console VT mode deletes ~60% of flinux's code. ConPTY itself is the wrong tool for guest ptys — measured, §7.3 |
| Recommendation | userspace line discipline in `linux_abi`, two backends, **no ConPTY**; new `hl_host_terminal_services` group of ~8 ops |
| Cost | **4–5 weeks** for the honest shipping subset; **8–9 weeks** for pty + job-control parity |
| Load-bearing measurement | `FILE_TYPE_CHAR` cannot answer `isatty` (`NUL` is also CHAR) — §7.2, so `metadata` alone is not enough and a new group *is* required |

A note on the premise. The brief cites "3,726 diagnostics, of which tty/termios is 169 across 89 symbols".
**Neither number exists on this branch** — `3,726` has zero hits in the tree. The repo's own sizing for this
work is `docs/windows/surface3-plan.md:187`: the **A-tty** migration unit, `ioctl` 14 + `isatty` 5 +
`tcgetattr`/`tcsetattr` 9 + `tc*pgrp` 4 = **32 call sites**, classed **greenfield**. That count is population-A
only (`syscall/{fs,io,net,event,binding}.c`); `surface3-plan.md:51` gives 15 `isatty` across all of
`src/linux_abi`. I have used the repo's numbers. The diagnostic count and the site count are different
denominators and should not be compared.

---

## 2. What flinux actually emulated

### 2.1 The `struct termios` is stored whole and honoured in eight bits

The layout is complete and correct — `src/common/termios.h:15-22`, `NCCS 19`, all 17 `c_cc` indices, every
`c_iflag`/`c_oflag`/`c_cflag`/`c_lflag` bit defined including the baud table and `CRTSCTS`. It is a faithful
copy of the Linux header.

The honouring is not. `TCSETS` is `memcpy(&console->termios, t, sizeof(struct termios))` —
`src/fs/console.c:1652-1661` — followed by:

```c
static void console_update_termios()
{
    /* Nothing to do for now */
}
```
— `src/fs/console.c:1631-1634`.

That empty function is the whole product decision. **The guest's termios never reaches the Windows console.**
`SetConsoleMode` is called exactly once, at init, with a constant — `console.c:246-247` —
`ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT` on input and `ENABLE_PROCESSED_OUTPUT` on output, and is never
touched again. Every flag that matters is instead re-read out of the stored struct at each `read`/`write` and
interpreted by flinux's own code.

Grepping every read of the structure across the whole tree (`console.c` is the only consumer) gives the exact
honoured set — **eight bits and two `c_cc` slots**:

| Field | Honoured | Where | Behaviour |
|---|---|---|---|
| `c_lflag & ICANON` | **yes** | `console.c:1348` | selects one of two entirely separate read loops |
| `c_lflag & ECHO` | **yes** | `console.c:1380,1390,1402,1514` | echo is emitted by flinux, not by the console |
| `c_iflag & ICRNL` | **yes** | `console.c:1370,1506,1508` | CR→NL on input |
| `c_iflag & IGNCR` | **yes** | `console.c:1369,1504` | drop CR |
| `c_oflag & ONLCR` | **yes** | `console.c:1586` | NL→CRNL on output |
| `c_oflag & OCRNL` | **yes** | `console.c:1578` | CR→NL on output |
| `c_cc[VMIN]`, `c_cc[VTIME]` | **yes** | `console.c:1415-1433` | full four-case Linux timing matrix, correctly |
| everything else | **accepted and ignored** | — | stored, returned by `TCGETS`, never consulted |

"Everything else" includes **`ISIG`, `IXON`/`IXOFF`, `IEXTEN`, `ECHOE`/`ECHOK`/`ECHOCTL`/`ECHOKE`, `ISTRIP`,
`INLCR`, `OPOST`, `TOSTOP`, `CSIZE`, `PARENB`, the baud rate, and `c_cc[VINTR]`/`VQUIT`/`VSUSP`/`VKILL`/
`VWERASE`/`VEOF`/`VEOL`.** Nothing is ever *refused*: `TCSETS` cannot fail. A guest that sets `-ISIG -IXON`
and checks with `TCGETS` sees its request honoured and gets none of the behaviour.

Two defects are visible in the honoured set itself. The default is `INLCR | ICRNL` together
(`console.c:214`), which is contradictory — `INLCR` maps NL→CR and `ICRNL` maps CR→NL. And
`console.c:1506-1509` reads

```c
if (ch == '\r' && console->termios.c_iflag & ICRNL) ch = '\n';
else if (ch == '\n' && console->termios.c_iflag & ICRNL) ch = '\r';
```

The second arm tests `ICRNL` where it plainly means `INLCR`; with the default mask both arms are live and NL
and CR swap each other. `INLCR` is therefore not merely unhonoured, it is mis-wired.

`VERASE` is initialised to `8` (`console.c:220`), not the `0x7f` that every modern Linux terminal uses. It is
never read anyway — erase is keyed off `VK_BACK` (`console.c:1385`).

### 2.2 The `ioctl` surface is seven requests

`console_ioctl` — `src/fs/console.c:1636-1707` — the complete list:

| Request | Line | Behaviour |
|---|---|---|
| `TCGETS` | `1644-1650` | `memcpy` out of the shared struct |
| `TCSETS` / `TCSETSW` / `TCSETSF` | `1652-1661` | all three identical; `/* TODO: What is the different between S/SW/SF variants? */` — `console.c:1641` |
| `TIOCGPGRP` | `1663-1669` | `log_warning("Unsupported TIOCGPGRP: Return fake result.")`, returns the caller's own pgid |
| `TIOCSPGRP` | `1671-1676` | `log_warning("Unsupported TIOCSPGRP: Do nothing.")`, returns 0 |
| `TIOCGWINSZ` | `1678-1690` | `GetConsoleScreenBufferInfo`, `srWindow` extent; `ws_xpixel`/`ws_ypixel` = 0 |
| `TIOCSWINSZ` | `1692-1698` | actually resizes the Windows window — `console_set_size`, `console.c:445-472` |
| anything else | `1700-1703` | **`-EINVAL`** |

`src/common/ioctls.h` defines all 60-odd Linux tty requests; seven are implemented. Note the fall-through is
`EINVAL`, not `ENOTTY` — Linux returns `ENOTTY` for an unknown request on a tty, and glibc's `isatty(3)` is
`tcgetattr` so it works, but code that probes with an unimplemented request gets the wrong errno.

**Not present anywhere in the tree**: `TIOCSCTTY`, `TIOCNOTTY`, `TIOCGSID`, `TCFLSH`, `TCXONC`, `TCSBRK`,
`TIOCOUTQ`, `FIONREAD` on a tty, `TIOCGPTN`, `TIOCSPTLCK`, `TIOCPKT`. There is **no `/dev/ptmx`, no
`/dev/pts`, no pty of any kind, and no winpty**. `/dev/tty` and `/dev/console` are the same object —
`src/fs/devfs.c:37-38` maps both names to the one `console_desc`, so there is no notion of a *controlling*
terminal distinct from *the* terminal. `setsid()` is `log_error("setsid() not implemented.")` —
`src/syscall/process.c:722-725`.

The struct is passed straight out of the guest pointer with no validation — `console.c:1646`,
`console.c:1656`, `console.c:1680` all cast `arg` and `memcpy` — where the rest of their `ioctl` path does
check (`src/syscall/vfs.c:2013` uses `mm_check_read` for `FIONBIO`). Not our problem, but it tells you how
much scrutiny this file got.

---

## 3. Line discipline: userspace, and only half of one

**They wrote it themselves.** They did not use the Windows console's own cooked mode. `ENABLE_LINE_INPUT` and
`ENABLE_ECHO_INPUT` are never set — `console.c:246` sets neither — and all input arrives as raw
`INPUT_RECORD`s via `ReadConsoleInputA`.

The canonical branch — `console.c:1348-1412` — is a 60-line loop over key-down records with a
256-byte line buffer (`MAX_CANON`, `console.c:52`). It handles exactly three cases:

- `VK_RETURN` — terminate the line, apply `IGNCR`/`ICRNL`, echo CRNL, return.
- `VK_BACK` — decrement, and if `ECHO` erase one cell (`backspace(TRUE)`, `console.c:416-432`).
- `default` — **`if (ch >= 0x20)`** append and echo.

That last guard is the whole story. In canonical mode **every control character is silently dropped**.
Ctrl+D does not produce EOF — `VEOF` is initialised to 4 (`console.c:221`) and never read, and `0x04` fails
the `>= 0x20` test. Ctrl+U (`VKILL`), Ctrl+W (`VWERASE`), Ctrl+R (`VREPRINT`), Ctrl+V (`VLNEXT`) and `VEOL`
do not exist. There is no erase-at-line-start protection beyond `console->x > 0` (`console.c:418`), so
backspacing past a wrapped line's start silently stops. There is no `ECHOE`/`ECHOCTL` distinction even
though `ECHOCTL` is in the default `c_lflag` (`console.c:217`).

The non-canonical branch — `console.c:1413-1527` — is the better half. `VMIN`/`VTIME` are handled with the
correct four-case matrix, `VTIME` is converted to milliseconds as `vtime * 100` (`console.c:1424`, correct —
`VTIME` is deciseconds), and interruption returns `-EINTR` only when nothing has been read yet
(`console.c:1428-1432`), which is right. Special keys are translated to xterm escapes by a hand-written
table, `console.c:1451-1499` — arrows with `DECCKM` awareness, Home/End, Insert/Delete/PgUp/PgDn, F1–F20.

**`ISIG` is not implemented at all.** Signal generation does not go through the line discipline; it is a
Win32 console control handler — `console.c:140-150` — installed once at init and re-installed after fork
(`console.c:274-277`). It maps `CTRL_C_EVENT` → `SIGINT` and nothing else. Consequently:

- **`-ISIG` does not work.** `ENABLE_PROCESSED_INPUT` is on permanently, so a guest that clears `ISIG` to
  read `0x03` as a byte gets a `SIGINT` instead. Every full-screen editor, pager and readline app that binds
  Ctrl+C in raw mode is wrong here.
- **`c_cc[VINTR]` is decorative.** Rebinding INTR to any other character has no effect.
- **Ctrl+Z is nothing.** `SIGTSTP` is not generated, and `signal_default_handler` — `src/syscall/sig.c:104-124`
  — has no `SIGTSTP`/`SIGTTIN`/`SIGTTOU`/`SIGCONT` case at all, so even a delivered stop signal is discarded.
- **Ctrl+\\ is nothing.** `CTRL_BREAK_EVENT` is explicitly refused: `if (dwCtrlType != CTRL_C_EVENT) return FALSE`
  — `console.c:142`.
- The `SIGINT` goes to `process_get_pid()` — **the receiving process only** (`console.c:148`), not to a
  foreground process group. In a pipeline only whichever process the console picked is interrupted.

`IXON`/Ctrl+S flow control: absent.

---

## 4. The terminal emulator they had to write

This is where the 1727 lines went. Because the Win32 console of 2014 had **no VT support at all**, guest ANSI
output could not be passed through — it would have landed in the cells as literal text. So flinux parses it
and replays it against the console API.

The parser is a function-pointer state machine — `console->processor`, `console.c:106` — dispatched per byte
in `console_file_write` (`console.c:1540-1624`), with handlers for CSI (`console.c:887-1130`), OSC
(`console.c:1133-1172`), `ESC #` (`console.c:1174-1195`), and G0/G1 charset designation
(`console.c:1197-1215`). What they implemented:

- **Cursor**: CUU/CUD/CUF/CUB/CHA/VPA/HPA/CUP/HVP, DECSC/DECRC, IND/NEL/RI.
- **Erase**: ED and EL, all three modes each.
- **Editing**: ICH/DCH/IL/DL, implemented with `ScrollConsoleScreenBufferW` (`console.c:550-564`).
- **Scrolling region**: DECSTBM, with a full software emulation of the margins — `scroll_top`,
  `scroll_bottom`, `scroll_full_screen` (`console.c:91-92`) — because the console has no such concept.
- **SGR**: `console.c:1036-1099` — reset, bold, faint, reverse, and the **8 foreground and 8 background
  colours only**, hand-mapped to `FOREGROUND_*`/`BACKGROUND_*` bits (`console.c:294-367`). No 256-colour, no
  24-bit, no underline, no blink, no italic. Anything else is `log_error("Unknown console attribute: %d")`.
- **Modes**: IRM and LNM; DECCKM, DECOM, DECAWM, 132-column, and the alternate screen buffer 47/1047/1048/1049
  (a second real `CreateConsoleScreenBuffer`, `console.c:213`).
- **Charsets**: DEC Special Graphics line-drawing, as a 32-entry Unicode table (`console.c:116-128`), plus SO/SI.
- **OSC**: title only (0 and 2).
- **DA2** answerback, faked as `\x1b[>61;95;0c` (`console.c:1022`); **DA1 is unimplemented** (`console.c:1029`).
- Their own UTF-8 decoder with a partial-sequence carry across writes (`console.c:93-94`, `console.c:642-670`)
  and `wcwidth` (a whole file, `src/wcwidth.c`) for double-width columns, plus manual wraparound and a
  correct `at_right_margin` deferred-wrap flag (`console.c:86`) — which the console does not model.

It is a real, competent VT100/xterm subset. It is also the thing that made this expensive: `console.c` is
**7.8% of flinux's 22,185 lines of C** and took 91 commits between 2014-08-16 and 2015-10-11.

One structural cost worth naming: because scrolling regions, cursor save-state, charsets and the emulated
`top` row are pure fiction, **all of it must be shared across every process in the console**. So
`struct console_data` lives in an `NtCreateSection` shared view mapped into each child at fork
(`console.c:153-272`) and every read and write takes a cross-process mutex (`console.c:279-287`). The
emulator's statefulness is what forced the shared-memory design, not the tty semantics.

---

## 5. Job control: abandoned, and the guest noticed

flinux reached the same conclusion `docs/windows/signals-and-faults.md:931-937` reaches for us. `TIOCGPGRP`
returns a fake, `TIOCSPGRP` is a no-op, both with a `log_warning` (`console.c:1665,1673`); `setsid` is
unimplemented (`process.c:722-725`); `TIOCSCTTY` does not exist; `/proc/<pid>/stat`'s `tpgid` field is
hardcoded to 0 with `/* TODO */` (`process.c:601`); and `SIGTTIN`/`SIGTTOU` are never generated by anything.

They did keep the *bookkeeping*: `pgid` and `sid` are real per-process fields inherited across fork
(`process.c:250-252`, `:279-281`) and `setpgid`/`getpgid`/`getpgrp`/`getsid` all work (`process.c:505-571`).
So the data model exists and only the terminal's half is missing.

What a guest shell does about it: bash calls `tcgetpgrp` at startup, gets its own pgid back, concludes it is
already in the foreground, and proceeds — then `tcsetpgrp` silently succeeds for every job. The observable
result is that **job control appears to work and does not**: `&`, `jobs`, `fg` and `bg` run, but a background
process reading from the terminal is not stopped with `SIGTTIN` — it competes for input with the shell.
Ctrl+Z does nothing. Ctrl+C hits one arbitrary process rather than the foreground group. This is the
"plausible-looking lie" failure mode; the honest alternative would have been to return `ENOTTY` from
`TIOCGPGRP` so bash disables job control and says so. I have **not** verified the bash behaviour by running
it — flinux does not build here — this is reasoned from the code and from bash's `initialize_job_control`.

---

## 6. Cost, and what never worked

| | |
|---|---|
| `src/fs/console.c` | **1727 lines** (`console.h` 35) |
| Share of the project | 7.8% of 22,185 lines of C |
| Commits / span | **91 commits, 2014-08-16 → 2015-10-11** |
| Never implemented | ptys, `/dev/ptmx`, `setsid`, controlling-terminal identity, `TIOCSCTTY`/`TIOCNOTTY`/`TIOCGSID`, `TCFLSH`, `TCXONC`, `ISIG`/`VINTR`, `IXON`, Ctrl+Z, Ctrl+D in canonical mode, `VKILL`/`VWERASE`/`VLNEXT`/`VEOL`, job control, 256/24-bit colour, DA1, mouse reporting, bracketed paste |
| Known-broken in-tree | `INLCR` mis-wired to `ICRNL` (`console.c:1508`); `TCSETSW`/`TCSETSF` = `TCSETS`; unknown ioctl → `EINVAL` not `ENOTTY`; input ring overflow undetected (`/* TODO */`, `console.c:616`); non-BMP characters dropped (`console.c:654`) |
| Structural cost | shared section + cross-process mutex on every console read/write, forced by the emulator's state |

The `log_error` call sites are a good census of the ragged edge: eight distinct "unhandled character" or
"not supported" paths in the escape parser alone (`console.c:803, 882, 1029, 1127, 1170, 1192, 1276, 1608`).

---

## 7. What Windows offers now that it did not in 2015 — measured

flinux predates every relevant API. Console VT support landed in Windows 10 1511 (Nov 2015, one month after
console.c's last commit) and ConPTY in 1809 (Oct 2018).

### 7.1 conhost is the terminal emulator now

Measured on 10.0.26200.8457. `scratchpad/exp_conmode.c`, `exp_conmode2.c`, `exp_conpty.c`.

**Both VT flags set cleanly.** From a fresh `AllocConsole()`, default modes are `CONIN 0x1f7`
(`PROCESSED|LINE|ECHO|…`) and `CONOUT 0x003`. `SetConsoleMode(out, |ENABLE_VIRTUAL_TERMINAL_PROCESSING)`
returns 1, `SetConsoleMode(in, ENABLE_VIRTUAL_TERMINAL_INPUT)` returns 1, readback `0x200` / `0x007`.

**VT output is interpreted, and correctly.** With `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, writing
`ESC[2J ESC[H ESC[1;31m R ESC[0;32m G ESC[0m N` put `RGN` at the viewport home with cell attributes
`0x000c 0x0002 0x0007` — bright red, green, default. `ESC[10;20H` + `X` left the cursor at viewport
`(20, 9)` and the character `X` at viewport `(19, 9)`. **With the flag off**, the same bytes land in the
cells as literal text — the first eight cells read back `<1b>[1;31mZ`. So the flag, and only the flag,
decides whether flinux's entire §4 emulator is needed. It is not.

*Coordinate subtlety, measured:* VT positioning is **viewport-relative**, so `srWindow.Top` must be added to
convert a VT row to a screen-buffer row. My first run of this test read buffer `(0,0)` and got `0x0007` for
all three cells — a false negative. Any code mixing VT output with `ReadConsoleOutput*`/`SetConsoleCursorPosition`
has to do that conversion.

**VT input produces exactly the xterm sequences flinux hand-wrote.** Injecting key records with
`WriteConsoleInputW` under `ENABLE_VIRTUAL_TERMINAL_INPUT` and reading with `ReadFile`:

| Key | Bytes | Key | Bytes |
|---|---|---|---|
| `a` | `a` | Delete | `ESC[3~` |
| Up / Down | `ESC[A` / `ESC[B` | PageUp | `ESC[5~` |
| Right / Left | `ESC[C` / `ESC[D` | F1 | `ESC O P` |
| Home / End | `ESC[H` / `ESC[F` | F5 | `ESC[15~` |
| Enter | `0d` | Backspace | **`7f`** |
| Tab | `09` | Shift+Tab | `ESC[Z` |
| Esc | `1b` | Alt+x | `ESC x` |
| Ctrl+D | `04` | Ctrl+U | `15` |
| Ctrl+C | **`03`** | Ctrl+S / Ctrl+Q | `13` / `11` |
| Ctrl+\\ | `1c` | Ctrl+Z | **0 bytes** |

Two things to read off that table. Backspace is `0x7f`, matching modern Linux `VERASE`, where flinux emitted
`0x08`. And **with `ENABLE_PROCESSED_INPUT` clear, Ctrl+C arrives as the literal byte `0x03` and the
`SetConsoleCtrlHandler` handler is not called** (`CTRL_C_EVENT` count 0). That is exactly `-ISIG` raw mode,
and it is the thing flinux could not do.

*Not measured, and it matters:* I could not exercise the `PROCESSED_INPUT`-**on** path. Records injected with
`WriteConsoleInput` bypass whatever layer converts Ctrl+C to `CTRL_C_EVENT` — with the flag on I still
observed the literal `0x03` and zero handler calls, which contradicts the documented behaviour and is
therefore an artefact of injection, not a result. The `ISIG`-on path needs a real keystroke to verify.
**Ctrl+Z produced zero bytes** in VT input mode under injection, with `uChar` both 26 and 0. I do not have an
explanation; treat `SIGTSTP` generation as unverified on this path.

**The classic cooked mode still exists and still works.** `LINE|ECHO|PROCESSED`, injecting `a b BS c CR`,
returned `ac\r\n` — the console did the line editing and appended CRLF. So `ICANON`+`ECHO`+`VERASE` are
available for free *if* you accept the console's editing rules and its `\r\n` terminator.

### 7.2 The three things VT mode does not give you

**(a) Resize is invisible in the byte stream.** Measured: with `ENABLE_VIRTUAL_TERMINAL_INPUT|ENABLE_WINDOW_INPUT`
set, shrinking the window queued exactly one input record, `EventType=4` = `WINDOW_BUFFER_SIZE_EVENT`, and a
subsequent `ReadFile` on `CONIN$` **timed out** — the record is neither converted to bytes nor consumed.
`GetConsoleScreenBufferInfo` did report the new `115x30`.

This is a hard architectural consequence, and it is the same one that shaped flinux's design. A console
input handle is signalled whenever its record queue is non-empty, but `ReadFile` only returns for records
that produce bytes. **So `WaitForSingleObject(CONIN$)` + `ReadFile` deadlocks the moment the window is
resized.** Any `poll`/`select`/`epoll` readiness answer for a tty must peek the record queue and discard
non-producing records before reporting `POLLIN` — which is precisely what flinux does in
`console_get_poll_status` (`console.c:1294-1320`). Their structure was forced, and it is still forced. The
practical shape is: read `INPUT_RECORD`s, not bytes, and either let conhost translate keys (impossible —
translation only happens on the `ReadFile` path) or use a dedicated thread that owns `ReadFile` and a second
mechanism for resize. **This is the single largest remaining piece of real work, and ConPTY does not remove it.**

`SIGWINCH` therefore has exactly one source on Windows: `WINDOW_BUFFER_SIZE_EVENT` from `ReadConsoleInput`.
Note `src/linux_abi/signal.c:238` currently reads `case 28: // SIGWINCH -- ignore`.

**(b) `ENABLE_LINE_INPUT` and `ENABLE_VIRTUAL_TERMINAL_INPUT` do not compose.** Measured: the same
`a b BS c CR` injection returned `ac\r\n` under `LINE|ECHO|PROCESSED` but `c\r\n` under
`LINE|ECHO|PROCESSED|VT_INPUT` — the `a` was lost. Separately, `LINE` **without** `ECHO` returned the raw
`ab<08>c<0d>` with no editing at all. I would not build on either combination. The usable configurations are
the two extremes: classic cooked (`LINE|ECHO|PROCESSED`, no VT) or fully raw (`VT_INPUT` alone).

Since `ICANON` and VT input are needed at different times by the same fd, and the console will not do both,
**a userspace canonical buffer is unavoidable** if `ICANON` is to be honoured while special keys still work.

**(c) `FILE_TYPE_CHAR` is not `isatty`.** Measured:

| handle | `GetFileType` | `GetConsoleMode` |
|---|---|---|
| pipe | `PIPE` | fails, `ERROR_INVALID_HANDLE` |
| regular file | `DISK` | fails |
| **`NUL`** | **`CHAR`** | **fails** |
| `CONIN$` / `CONOUT$` | `CHAR` | **succeeds**, `0x1f7` / `0x003` |

`src/host/windows/file.c:253-256` maps `FILE_TYPE_CHAR` → `HL_HOST_FILE_TYPE_CHARACTER`, so today a guest
`isatty()` answered from `metadata` alone would **return true for `/dev/null`**. The discriminator is
"`GetConsoleMode` succeeds", and that fact is not expressible anywhere in the current contract. This is the
measurement that decides §9's answer about a new group.

### 7.3 ConPTY: what it is, and why it is the wrong tool here

Measured with `scratchpad/exp_conpty.c` and `exp_passthru.c`. `CreatePseudoConsole(80x25)` →
`hr=0x00000000`; `UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE)` → 1; child spawned and
reported `size=80x25`. `ResizePseudoConsole(100x40)` → `S_OK`, and the child's next
`GetConsoleScreenBufferInfo` reported `100x40`. Input is clean: the host wrote `ESC[A hello` into the input
pipe and the child, having set `ENABLE_VIRTUAL_TERMINAL_INPUT` itself (readback `0x200` — **the flag works
inside a ConPTY**), read exactly `ESC[A`. So ConPTY does what it says.

But the child's default `CONIN` mode inside a ConPTY is `0x1f7` — the ordinary cooked console. **ConPTY hands
its client a console, not a byte stream.** And the output side is not a pipe at all:

> child wrote 25 bytes; pty emitted 202 bytes (**amplification 8.1×**)
> payload appears VERBATIM in pty stream: **NO (re-rendered)**

The child wrote `ESC[31m A ESC[0m B<3 spaces> ESC[?7777h | CRLF`. The pty emitted
`ESC[?9001h ESC[?1004h ESC[?7777h ESC[?25l ESC[2J ESC[m ESC[31m ESC[H A ESC[m B<spaces> | CRLF OSC0;<child exe path> BEL ESC[?25h`.
ConPTY **injected** win32-input-mode and focus-event enables and an OSC title carrying the child's full
executable path, **reordered** the SGR ahead of the cursor home, **rewrote** `ESC[0m` as `ESC[m`, and wrapped
everything in cursor-hide/show. On resize it repainted the entire 40-line screen with `ESC[K` per row (388
bytes for a 3-line screen). ConPTY is a **screen differ**: it renders its client into a cell buffer and emits
VT to reproduce that buffer on a terminal.

I tested the undocumented `PSEUDOCONSOLE_PASSTHROUGH` bit (`0x8`) — **byte-identical output to `flags=0` on
this build**, 202 bytes, still re-rendered. `PSEUDOCONSOLE_INHERIT_CURSOR` (`0x1`) emitted only `ESC[6n` and
then stalled: it blocks waiting for the terminal to answer a DSR cursor report, which my harness never sent.
The mingw header exposes only `PSEUDOCONSOLE_INHERIT_CURSOR` (`consoleapi.h:122`).

**Therefore ConPTY cannot implement guest `/dev/ptmx`.** A Linux pty is byte-transparent in both directions;
ConPTY is not, cannot be made so on this build, and would leak the host executable path into the guest's
master fd. It is the right tool for exactly one job — putting a *Windows* console program on the other end of
a guest pty (`cmd.exe` under a guest `script`, say) — which is not a requirement anyone has stated.

**What actually made this cheaper is not ConPTY, it is the console VT flags** — and those arrived in 1511,
three years before ConPTY. They delete §4 entirely: roughly 1,000 of flinux's 1,727 lines. They do not touch
§3, and §7.2(a) says the read path is still bespoke.

---

## 8. Our situation

Three facts about this tree, verified.

**The contract has nowhere to put any of this.** `include/hl/host_services.h` is 725 lines, 15 groups, 133
operations, and contains **zero** occurrences of `ioctl`, `termios`, `tty`, `isatty`, `winsize` or `tcget`.
The aggregate at `:699-718` lists every group; there is no terminal among them. The nearest thing is the file
type enumerator `HL_HOST_FILE_TYPE_CHARACTER = 4` (`:101`), which §7.2(c) measured as insufficient.

**The Linux front already has the full implementation — against libc.** `src/linux_abi/syscall/fs.c:1053-1506`
is far richer than flinux ever was: `TCGETS`/`TCGETS2`/`TCSETS{,W,F}{,2}` with a real Linux↔native
translation table (`container/netns.c:24-38`, `termios_l2m`/`termios_m2l` at `:172-195`), `TIOCGWINSZ`/
`TIOCSWINSZ`, `TCSBRK`, `TCSBRKP`, `TCFLSH`, `TCXONC`, `TIOCGPTN`, `TIOCSPTLCK`, `TIOCGPTPEER`, `FIONBIO`,
`FIONREAD`, `TIOCOUTQ`, `FIOCLEX`/`FIONCLEX`, `TIOCGPGRP`, `TIOCSPGRP`, `TIOCSCTTY`, `TIOCNOTTY`, `TIOCGSID`,
`TIOCPKT` — plus a pty-master retarget for macOS (`fs.c:1128-1165`), a devpts registry
(`container/vfs.c:6389-6448`), and `/proc/self/fd` rewriting so `ttyname(3)` sees `/dev/pts/N`
(`fs.c:3842-3859`). Unknown request → `-ENOTTY` (`fs.c:1480-1490`). **All of it calls `tcgetattr`,
`tcsetattr`, `isatty`, `ptsname`, `tcflow`, `tcflush` and host `ioctl(2)` directly.** None of that exists on
Windows. The typed lane's equivalent (`binding.c:3405-3503`) reaches termios only through
`bound_attachment_borrow` → a native fd, and returns `-EOPNOTSUPP` without it — the row already recorded at
`docs/windows/host-services-map.md:700` as a graceful degradation.

So this is not greenfield semantics. **The semantics are written; the substrate is missing.** That is a
materially cheaper problem than the brief assumes, and it also means the target behaviour is already pinned
by an existing compat corpus: `tests/compat/posix/` has 18 pty/tty programs with goldens — `ptycanon`,
`ptyldisc`, `termraw`, `ptyjobsig`, `ctty_session`, `tty_notty`, `tty_suspend`, `tty_leaderhup`,
`tty_bg_access` and more (`manifest.tsv:76,87-95`). **We can measure a Windows tty implementation against
the same goldens the POSIX hosts already pass.** That is worth more than any design argument in this document.

**The fd-lane provider vtable has no `ioctl` slot.** `src/linux_abi/object.h:14-34` — `read`, `write`,
`status`, `set_status_flags`, `readiness`, `wait_handle`, `subscribe`, `unsubscribe`, `retire`, `clone`,
`close`. No `ioctl`, no attributes. `docs/windows/prior-art-flinux-fs.md:197` already names this gap.

And `docs/windows/signals-and-faults.md:931-937` concluded job control is lost because Windows has no host
process groups. **That conclusion is about the host and is correct, but it does not bind the tty.** Once the
terminal is emulated in-process, foreground-pgid, `SIGTTIN`/`SIGTTOU` and `TIOCSPGRP` are guest-side state
between guest-side processes; they never needed a host process group. Job control on Windows is blocked on
guest-side `SIGSTOP`/`SIGCONT` and `WUNTRACED`, not on the tty. Those are separable, and the tty half is the
cheap half.

---

## 9. Recommendation

**Build a userspace line discipline in `src/linux_abi`, with two backends and no ConPTY. Add a small typed
`hl_host_terminal_services` group carrying bytes, mode, and size — and nothing Linux-shaped.**

The load-bearing decision is that **the line discipline is host-independent code**. It is a pure state
machine over byte streams: it does not call a Windows API, it is unit-testable without a console, and it is
the *same* code for a console-backed tty and for an internal pty pair. flinux's mistake was not writing a
line discipline; it was fusing the line discipline, the terminal emulator and the Win32 console into one
1727-line file with cross-process shared state. Split those three and two of them disappear.

### The four options, costed

Estimates are engineer-weeks including tests against `tests/compat/posix/`, and assume the existing
`fs.c`/`netns.c` semantics are the specification. I have **not** validated them against this team's velocity.

**(a) Minimal honest subset — `TIOCGWINSZ` + raw passthrough. ~1.5 weeks, ~350 lines.**
`isatty` true; `TCGETS` returns a fixed raw-ish termios; `TCSETS` accepts and ignores; `TIOCGWINSZ` from
`GetConsoleScreenBufferInfo`; VT flags on both handles; unknown request → `ENOTTY`.
*What breaks:* everything that needs `ICANON`. A shell has no line editing — no backspace, no history, no
readline, because readline sets raw mode and then relies on `ISIG` and on the terminal not echoing. Anything
calling `tcsetattr` to turn echo **off** (`sudo`, `ssh`, `passwd`, `getpass`) leaks the password to the
screen — that is a security failure, not a cosmetic one, and it alone disqualifies (a) as a shipping state.
`vi`, `less`, `top`, `htop`, `python` REPL, `git` interactive: all broken or dangerous.
Useful only as a two-day scaffold to get a shell to a prompt.

**(b) ConPTY-backed real pty. Not recommended. +2–3 weeks and a permanent fidelity loss.**
Measured in §7.3: 8.1× amplification, non-transparent, injects the host exe path, `PASSTHROUGH` inert on
10.0.26200. It buys exactly one capability — a Windows console program on the far end of a guest pty — which
nothing has asked for. **The right implementation of `/dev/ptmx` is a pure in-process byte-pair plus the same
line discipline**, ~700 lines, no Windows API, and as a bonus it is host-portable: it would also retire the
macOS pty-master workaround at `fs.c:1118-1165` and the `openpty` dependency at `src/core/activation.c:823-835`.
Revisit ConPTY only if a concrete "run `cmd.exe` inside the guest" requirement appears.

**(c) Full userspace line discipline — recommended, but not flinux-style. ~4–5 weeks, ~1,500 lines.**
Not flinux-style because §4 is deleted by the VT flags. Four pieces:

| Piece | Lines | Weeks | Notes |
|---|---|---|---|
| Line discipline core | ~600 | 1.5 | canonical buffer, `VERASE`/`VWERASE`/`VKILL`/`VLNEXT`/`VREPRINT`/`VEOF`/`VEOL`, `ECHO*` family, `ISIG` + `VINTR`/`VQUIT`/`VSUSP`, `IXON`, `VMIN`/`VTIME`, `OPOST`/`ONLCR`/`OCRNL`/`ONLRET`. Pure; no host calls; testable headless |
| `ioctl` plumbing | ~400 | 1 | append an `ioctl` (or `attributes`) callback to `hl_linux_object_ops` — the same append convention `host_services.h:239,411` uses; per-OFD termios record; ~20 requests; unknown → `ENOTTY` |
| Console backend | ~500 | 1.5–2 | mode mapping; a dedicated input thread owning `ReadConsoleInput`; `WINDOW_BUFFER_SIZE_EVENT` → `SIGWINCH`; the §7.2(a) peek-before-`POLLIN` rule. **This is the risk-bearing piece** |
| Wiring + corpus | — | 0.5–1 | bind fds 0/1/2 as tty objects at `src/core/target/run.c:71-97`; run `ptycanon`, `ptyldisc`, `termraw` |

*What this buys:* echo-off works, so passwords are safe; `ICANON` line editing works; `-ISIG` raw mode works
(measured available, §7.1); readline, `vi`, `less`, `top` and the Python REPL work; `SIGWINCH` works; `isatty`
is honest. *What it still does not buy:* ptys and job control.

**(d) Add the pty pair and the ctty/job-control bookkeeping. +3–4 weeks, +~1,100 lines.**
`/dev/ptmx`, `/dev/pts/N`, devpts, `TIOCGPTN`/`TIOCSPTLCK`/`TIOCGPTPEER`/`TIOCPKT` (~700 lines, 2 weeks) —
the same line discipline, a byte queue on each side, no Windows API. Then session/ctty state, `TIOCSCTTY`/
`TIOCNOTTY`/`TIOCGSID`/`TIOCGPGRP`/`TIOCSPGRP` and real `SIGTTIN`/`SIGTTOU` generation (~400 lines,
1–1.5 weeks) — all guest-side, per §8. Unblocks `ssh`, `sudo`, `expect`, `script`, `screen`/`tmux`, and
Python's `pty` module. Job control still needs guest-side `SIGSTOP`/`SIGCONT` and `WUNTRACED`, which is
`signals-and-faults.md`'s work, not this.

**Recommendation: (c) now, (d) next, never (b).** **4–5 weeks** to a defensible shipping state; **8–9 weeks**
to parity with what `fs.c` already does on POSIX. So the measuring agent's instinct was right — it is more
than two weeks — but the reason is the line discipline and the §7.2(a) read path, not a terminal emulator,
and the ceiling is roughly half of what flinux's 1727 lines and 14 months would imply.

### Yes, the contract needs a new group

`metadata` cannot answer `isatty` — measured, §7.2(c): `NUL` is `FILE_TYPE_CHAR`. This settles the open
question at `docs/windows/surface3-plan.md:497-499` with a fact rather than a preference.

The group must be **typed and Linux-free**. Do not put `ioctl(fd, request, arg)` in the contract: it would
push Linux request numbers and `struct termios` layout into every host, and every one of the 15 existing
groups is typed precisely to avoid that. The split that follows from §7 is:

- **Host provides**: is-this-a-terminal, raw byte in/out, the console's own mode bits, window size, and a
  size-change signal. All facts about the *device*.
- **`linux_abi` owns**: `struct termios`, `c_cc`, canonical buffering, echo, signal generation, `VMIN`/`VTIME`,
  `OPOST`. All facts about *Linux*.

Sketch, following the file's conventions (`HL_HOST_TERMINAL_ABI 1u` in the block at `:8-29`;
`HL_HOST_CAP_TERMINAL = UINT64_C(1) << 18` — the next free bit, `1 << 17` being the highest in use at `:52`;
struct appended to `hl_host_services` at `:699-718`; a validation clause in `src/core/host_services.c`
modelled on the `WATCH` clause at `:142-147`):

```c
typedef struct hl_host_terminal_services {
    HL_ABI_HEADER;
    /* value is nonzero when handle is an interactive terminal.  Distinguishes a
       console from other FILE_TYPE_CHAR devices, which metadata cannot. */
    hl_host_result (*probe)(void *context, hl_host_handle handle);
    hl_host_result (*get_mode)(void *context, hl_host_handle handle, uint32_t *mode);
    hl_host_result (*set_mode)(void *context, hl_host_handle handle, uint32_t mode);
    hl_host_result (*get_size)(void *context, hl_host_handle handle, hl_host_terminal_size *size);
    hl_host_result (*set_size)(void *context, hl_host_handle handle, hl_host_terminal_size size);
    /* Raw bytes, already VT-translated by the host.  Never blocks on a record that
       produces no bytes -- see the resize hazard. */
    hl_host_result (*read)(void *context, hl_host_handle handle, hl_host_bytes output);
    hl_host_result (*write)(void *context, hl_host_handle handle, hl_host_const_bytes input);
    /* Borrowed host object signalled on size change; drives SIGWINCH. */
    hl_host_result (*size_change_event)(void *context, hl_host_handle handle);
} hl_host_terminal_services;
```

Eight operations, none Linux-shaped, all four hosts implementable — Linux and macOS map `probe` to `isatty`,
`get_size`/`set_size` to `TIOCGWINSZ`/`TIOCSWINSZ`, `get_mode`/`set_mode` to a small abstract raw/cooked/echo
mask, and `size_change_event` to a self-pipe fed by `SIGWINCH`; the fake host stubs per
`docs/windows/linux-abi-fd-lane.md:356-358`. The `mode` word should be an `HL_HOST_TERMINAL_*` bitmask of
abstract capabilities (raw, echo, signals, flow-control), **not** a passthrough of `SetConsoleMode` bits.

`hl_linux_object_ops` (`src/linux_abi/object.h:14-34`) also needs one appended callback so a tty object can
answer `ioctl`. That is an internal struct, not the published contract, so the cost is a recompile.

---

## 10. What I did not measure

- **The `ENABLE_PROCESSED_INPUT`-on path.** §7.1: `WriteConsoleInput` injection bypasses the layer that
  raises `CTRL_C_EVENT`, so I proved the raw path and not the cooked one. Needs a real keystroke or a driven
  ConPTY. The `ISIG`-on design depends on it.
- **Ctrl+Z under VT input.** Measured as zero bytes, twice, with no explanation. `SIGTSTP` generation is
  unverified.
- **Behaviour under Windows Terminal versus bare conhost.** All measurements used a fresh `AllocConsole()`.
  Windows Terminal is a ConPTY consumer and the client-side flags should be identical, but that is reasoned,
  not measured.
- **Whether bash actually degrades the way §5 says under flinux.** flinux does not build here; §5's guest
  behaviour is reasoned from their code and from bash's `initialize_job_control`, not observed.
- **Any performance figure.** No throughput or latency number for console I/O appears in this document,
  because none was taken. flinux's cross-process mutex on every read and write is described as a structural
  cost, not benchmarked.
- **`PSEUDOCONSOLE_PASSTHROUGH` on builds other than 10.0.26200.8457.** It was inert here. It may not be
  elsewhere; the recommendation against ConPTY does not rest on it, since re-rendering is inherent to what
  ConPTY is for.
