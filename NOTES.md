# ffanim notes

Why the pinned block behaves the way it does, and what it costs.
The [README](README.md) is the short version.

## How the pin works

`--pin` clears the screen, sets a scroll region (DECSTBM) below the block so
the top rows never scroll, paints one static frame, puts the cursor below the
region, and forks. The parent exits immediately so the shell gets its prompt;
the detached child repaints rows 1..N forever.

Two things here are easy to get wrong.

DECSTBM homes the cursor, so re-arming the region mid-run yanks the prompt back
to the top of the screen. The region change has to sit inside a DECSC/DECRC
(`ESC 7` / `ESC 8`) pair, and should only happen on an actual resize.

Stale pidfiles are dangerous, because both pids and pts numbers get recycled
and a leftover pidfile can name an unrelated process. Check
`/proc/<pid>/cmdline` before signalling anything.

The painter polls the terminal size each frame rather than handling SIGWINCH:
after `setsid()` it is not in the foreground process group and would never
receive the signal. It exits when a write to the tty fails, which is what
happens when the terminal closes, and removes its pidfile on the way out.


## Full-screen apps

The scroll region is something the shell honours. A TUI does not have to. vim,
less, htop and a coding agent all draw wherever they like, rows 1..N included.
Keep repainting through that and the block sits on top of somebody else's UI.

The obvious signal would be the alternate screen, and it fails twice. It is
unreadable: the alternate screen lives in the terminal emulator, the pty carries
no trace of it, and asking the terminal means reading from the tty, which would
eat the keystrokes the foreground app is waiting for. It is also not a reliable
marker. `top`, `btop`, `watch`, `gdb` and a plain `python3` prompt all take the
screen without ever switching to it.

What they do have in common is the keyboard. So the painter reads two things it
can get without touching anyone's input:

- the foreground process group, from `tpgid` in `/proc/<session leader>/stat`.
  `tcgetpgrp` would be the obvious call and it is unavailable here, for the
  same ENOTTY reason as in [Resizing](#resizing).
- canonical mode, from `tcgetattr` on the tty it already holds open.

A foreground group that is not the shell's, holding the terminal out of
canonical mode, owns the screen. The painter then writes nothing at all until
it is gone, not a wipe and not a region reset, since every one of those bytes
would land on the other app's screen. On the way back it re-arms the scroll
region, which the app will have reset, and repaints.

The shell's own line editor is raw too, but it is the foreground group itself,
which is what `tpgid` separates. That is also what keeps ordinary commands
animating: `make`, `git status`, anything that prints and scrolls leaves
canonical mode alone, and so does a password prompt, which only drops `ECHO`.

Measured against what is actually installed here, one sample per second while
the program runs:

| holds the painter | keeps animating |
|---|---|
| vim, nano, less, man, bat, git log | ls, make, git status, cat |
| top, btop, watch, cava, whiptail, fzf, glow | sleep, `yes \| head` |
| python3 and node REPLs, gdb, a nested shell | `read -s` password prompts |

The right column is the useful half: you do not have to turn the animation off
to get work done. The left column holds more than it strictly needs to, since a
REPL is not full-screen and could be painted over safely. But a REPL has the
keyboard, and the keyboard is all the painter can see.

Two things it does not catch.

A full-screen program that never reads the keyboard leaves canonical mode
alone, so it reads as ordinary output and gets painted over. Nothing in the pty
distinguishes it. That one is a real limitation rather than something to tune.

A program that crashes without restoring the terminal leaves canonical mode off
behind it. The shell hands that state to every command after it, and the painter
reads the terminal as permanently busy. The same wreckage is why your shell
stops echoing, and `reset` fixes both.


## Running your shell inside ffanim

`--pin` paints the top rows of the live screen, which is why the block never
enters the terminal's buffer and never scrolls. Making it scroll means knowing
which row it is on after the screen has moved, and a detached process cannot
know that. The terminal never says, and asking with a cursor position report
means reading the tty, which steals the keystrokes the shell is waiting for.

`--wrap` removes the question by owning the stream. ffanim opens a pty, starts
your shell on it, and relays both directions. Every byte the shell writes passes
through ffanim first, so it can count exactly how far the screen has scrolled.

It counts by tracking the cursor: line feeds, carriage returns, tabs, wraps at
the right margin, and the cursor movement sequences. When the cursor would pass
the last row, the screen has scrolled, and that is where the animation ends.

It could keep going, painting the block at wherever the scroll left it, and an
earlier version did. That is a bad trade. Animating a half scrolled block buys
very little, and an offset that is wrong by one row paints the block over itself
and leaves that wreckage in your scrollback for good. Stopping at the first
scroll means ffanim only ever paints at the top of the screen, where it cannot
be wrong, and what ends up in your history is exactly what was printed.

The tracking is deliberately cowardly. Anything it cannot account for, an
unknown escape, a switch to the alternate screen, a resize, ends the animation
rather than risking a frame painted over someone else's output. That also bounds
the cost: the expensive path only runs while the block is still on screen, which
is the first few seconds of a session. After that ffanim is a plain byte relay.

Measured on this machine against 82 MB of colour-heavy output, every line
carrying an SGR sequence and an erase, best of five runs: the relay carries
108 MB/s where a bare pty carries 115 MB/s, and scanning the stream for line
feeds costs nothing next to the syscalls. A keystroke gains 1.8 us from the
extra hop. Neither is the real cost.
The real cost is that your shell's parent is now ffanim, so a crash takes the
session with it, and that is a much larger blast radius than a painter you can
kill without consequence.

## Resizing

A scroll region stops the shell from scrolling the block away. It does not
protect the block from a resize. Terminals reflow text when the window
changes: foot pushes the top rows into the scrollback when the window gets
shorter, and pulls them back, shifting everything down, when it grows again.
The block is ordinary grid content and rides along, and the region itself is
dropped on the way.

A repaint alone therefore is not enough. Paint the new block at the top and the
old one is still wherever the reflow carried it, a second copy sitting in the
shell's rows below the live one. Small terminals show it worst, because there
the block is most of the screen.

Erasing only that copy would be the tidy answer, and it is not available. How
far the content moved depends on how much scrollback there was to pull back,
which the painter cannot know, and a terminal with nothing to pull does not
move the content at all. From outside, a displaced block and a screen of
command output look identical: both are rows the painter did not write. Reading
the screen back would settle it, and reading the tty steals the keystrokes the
foreground program is waiting for, the same wall as in
[Full-screen apps](#full-screen-apps).

So a resize is a re-pin rather than a repaint: clear the visible screen, re-fit
the art to the new size, re-arm the region, redraw, and ask the shell for its
prompt back. Nothing stale survives because nothing survives. The scrollback is
left alone, so anything that had scrolled off is still there to scroll back to;
what a resize costs you is the screenful you were looking at. That is worth
knowing before you wire this into a shell.

Re-arming has to sit inside a DECSC/DECRC (`ESC 7` / `ESC 8`) pair when it
happens outside a clear, because DECSTBM homes the cursor and would otherwise
yank the prompt to the top of the screen.

Asking for the prompt back is its own problem. Plain SIGWINCH does not do it:
shells redraw on a size *change*, and the size has not changed since the
shell's own redraw. So the painter sets the window size one row short and
immediately back with TIOCSWINSZ. The kernel signals the foreground process
group on each write, and the second one lands with the real size, so the shell
repaints correctly. Note that `tcgetpgrp` is useless here: after `setsid()` the
painter has no controlling terminal and it fails with ENOTTY. TIOCSWINSZ has no
such requirement, and the kernel picks the process group itself.

That repaint is what `FFANIM_PROMPT_LEAD` is for. A repaint differs from a
fresh print: printing a prompt runs whatever leading newline it carries, while
repainting one starts from the cursor and walks back up by however many rows
the prompt occupies. Park the cursor where the prompt began and the repaint
lands above it. `FFANIM_PROMPT_LEAD` is how far below the region's first row to
park so that the repaint lands exactly where a fresh prompt starts. The default
2 suits a prompt with one leading blank line, like starship with `add_newline`;
a single-line prompt wants 0.

When the window gets too small to hold the block the painter wipes its rows,
releases the region and *pauses* rather than exiting, then re-pins once there
is room again. Exiting would mean one careless resize kills the animation for
the life of the terminal.

Rows are also clamped to the current width and height, counting columns rather
than bytes, so a frame can never wrap or spill past the last row.

## Fitting a small terminal

When the block does not fit, ffanim shrinks it rather than giving up. Two
steps, in order:

1. Blank lines are dropped from the top and bottom of the info pane. Those are
   usually `break` modules used to centre the info against a tall logo, and
   that centring is moot once the logo has to shrink anyway.
2. Braille art is rescaled. The art is decoded back into its dot bitmap (two
   dots per cell across, four down), box-filtered down, and re-encoded, so the
   aspect ratio holds and thin strokes survive better than dropping rows would
   allow. Non-braille logos are left alone, since a text logo cannot be
   resampled this way.

A 181x14 terminal, for example, takes a 40x15 logo down to 32x12.

What cannot shrink is the info pane itself: one line per fastfetch module. If
those lines alone do not leave room for a usable shell, `--pin` execs plain
fastfetch, since a one-shot print may scroll where a pinned block may not. Run
manually, ffanim says what it needs and exits. With no controlling terminal at
all it prints a static frame.


## Refreshing the info pane

The pane is a snapshot of the moment the block was pinned. Leave the terminal
open for an afternoon and it still reports the uptime and memory of that
moment, which is worse than showing nothing: it looks current and is not.

`--refresh <seconds>` fixes it, and cannot do so by simply re-running fastfetch
and swapping the pane in. The pane ffanim gets back is not the one your shell
produced. fastfetch names your shell and terminal by walking the parent process
chain, and from a painter that has called `setsid()` that chain leads nowhere
useful, so a wholesale swap rewrites those lines with whatever the painter
looks like from outside. In testing it turned a Shell line reading `fish 4.9.3`
into an empty one.

So the refresh compares ffanim's own runs against each other rather than
against the screen. Lines that differ between two consecutive probes are the
ones that move on their own: uptime, memory, disk. Those are the only lines
replaced. Everything else keeps the text your shell piped in, which is the only
copy that was ever correct. The first tick is a baseline and changes nothing,
so with `--refresh 30` the first update lands at a minute.

If a probe comes back with a different number of lines than the pane on screen,
nothing is patched at all. That is not a rare guard. It fires whenever ffanim's
config and your shell's disagree, including through an alias ffanim cannot see,
and it fires on fastfetch's default config, whose Colors block is a different
height when the process has no terminal to measure.

It is off by default. A probe is a fastfetch run, and a config carrying a module
that goes to the network would stall the animation for as long as that takes.

## The four animations

Everything the animation does comes out of one function of two numbers, the row
and the position of the light:

    lit_at(row, pos)

`sweep` is `1 - |row - pos| / SPAN`, one band with a hard edge. `wave` is
`0.5 + 0.5 sin((row - pos) * 0.6)`, several bands at once and never fully dark.
`pulse` drops `row` entirely and leaves `0.5 + 0.5 sin(pos * 0.5)`, so the whole
logo moves together. `bounce` shares sweep's shape and only changes how `pos`
advances, turning around at the ends instead of wrapping. Four behaviours, three
lines of arithmetic, because the hard part was already there: one brightness per
row, one SGR sequence per row.

An earlier fifth, `ember`, added a slow global term to wave. It measured 0.92
correlation against wave over 400 frames, which is another way of saying it was
wave, so it went.

`glitch` is the one that leaves brightness alone. It works on the dots:
`scale_logo` already decoded the braille into a bitmap in order to rescale it,
so that bitmap was sitting there unused the rest of the time. Each dot gets a
fixed order from a hash of its coordinates, and a band travelling down the logo
knocks out the dots whose order falls under the band's bite, hardest at the
centre and not at all beyond `SPAN`. Behind the band they come back. Nothing is
random per frame, so the same `pos` always draws the same logo.

It is the cheapest of the six rather than the dearest, 9.9 KiB/s against sweep's
13.6, because only the rows the band is crossing differ from the frame before
and the row cache drops the rest.

`ripple` uses the same bitmap for the opposite kind of change. Instead of
removing dots it moves them: every dot row is read back at an offset of
`3 sin((row - pos) * 0.5)` dot columns, so the logo bends as the wave travels
down it. That touches every row the wave covers, which is most of them, so it
lands at 32.4 KiB/s, between the band animations and the ones that relight
everything.

A logo that is not braille has no dots to knock out or slide, so both render it
steady rather than blank.

The freeze in `--wrap` is worth one line here: when the screen scrolls, ffanim
now paints one last frame with the animation flat before it stops, so what ends
up in the scrollback is the whole logo rather than whatever the band happened to
be eating at that moment.

That structure is also the limit. A diagonal or a left to right wipe cannot be
one colour per row, so `row_text` would have to walk braille cells and emit a
colour every few columns. Ten times the bytes per frame for a much larger change
than all four of these together, so it is not in.

The row cache is what makes the difference visible in the numbers. The painter
sends only rows whose text changed, and the two directional animations change
three or four rows per frame while the other two change all of them. Measured on
a 16 row logo at 20 fps, pinned:

| | to the tty |
|---|---|
| `sweep` | 13.6 KiB/s |
| `bounce` | 12.7 KiB/s |
| `wave` | 42.4 KiB/s |
| `pulse` | 42.4 KiB/s |
| `glitch` | 9.9 KiB/s |
| `ripple` | 32.4 KiB/s |

Three times more for the two that defeat the cache, and still three orders of
magnitude below what the relay carries.

## Choosing, and switching off

`--anim` and `--color` save and exit rather than applying to one run. The reason
is the shell config: if they were per-run flags, every change would mean editing
the snippet you pasted months ago, and that snippet is the one thing that should
never need touching again. They write a word into `~/.config/ffanim/anim` and
`~/.config/ffanim/color`, and ffanim reads them at startup. A value it does not
recognise is ignored rather than fatal, because a damaged file should not be
able to stop your greeting from appearing.

`--off` is the same idea taken further: an empty file at `~/.config/ffanim/off`.
When it exists, `--wrap` prints the block and then `exec`s your shell instead of
building a pty and relaying, so ffanim leaves the process tree entirely and the
cost is not reduced but zero. It reopens stdin from `/dev/tty` before the exec,
because in the pipeline that stdin is the fastfetch pipe and a shell handed an
exhausted pipe exits immediately.

Neither touches your shell config, because neither has to. `--setup` is the one
that writes there, and it earns it by being reversible: a block between two
markers, appended to the end, with the old file copied to `<file>.ffanim.bak`
first. `--unsetup` cuts exactly that block back out, which is also what
`--uninstall` calls, so nothing is left behind pointing at a binary that is
gone.

## Cost, measured against fastfetch

The question is what the animation adds to a `fastfetch` you were going to run
anyway. Two separate costs: the greeting you wait for, and what stays behind.

Measured on an i5-13420H, foot-sized 120x40 pty, 14 fastfetch modules, 40x15
braille logo. 40 runs per case, median.

**Greeting, what your shell waits for**

| | median | vs fastfetch |
|---|---|---|
| `fastfetch` | 25.5 ms | baseline |
| `fastfetch --logo none \| ffanim --pin --stdin` | 27.9 ms | +2.4 ms |
| `ffanim --pin` (ffanim spawns fastfetch) | 28.7 ms | +3.2 ms |

Those deltas are small enough that the medians wander by a millisecond or so
between runs; the ordering does not. Almost all of it is fastfetch either way.
ffanim's own share is 3.9 ms from exec to the parent returning, and in the
pipeline it overlaps fastfetch, so the pipeline costs less than the two added
up. `--pin` returns once the static frame is on screen; the animation is
already a detached child by then, so the prompt is not waiting on it.

**Resident, what keeps running**

fastfetch prints and exits, so its column is empty by definition. The painter
is the price of the animation.

| at 20 fps, 120x40 | fastfetch | ffanim painter |
|---|---|---|
| resident memory | nothing | 1.6 MB RSS |
| CPU | nothing | 0.22 % of one core |
| writes to the tty | nothing | 21 KiB/s |

Measured over 60 s from `/proc/<pid>/stat` and `VmRSS`. It scales with `--fps`
about as you would expect:

| `--fps` | CPU | tty writes |
|---|---|---|
| 10 | 0.10 % | 10.9 KiB/s |
| 20 (default) | 0.22 % | 21.3 KiB/s |
| 30 | 0.30 % | 31.5 KiB/s |

The painter's own CPU is not the whole bill: your terminal emulator parses
those bytes and redraws 20 times a second. `--fps 10` halves both sides and
still looks like an animation.

The write rate used to be three times that. The band lights a row and its four
neighbours, so with a 16 row logo only about six rows differ from one frame to
the next, and the info pane beside them never differs at all. Measured over a
full sweep: 5.7 of 16 rows change per frame, 65 % of what was being sent
carried no new pixels. The painter now keeps the bytes it last put on each row
and skips the rows that would not change, which took 65 KiB/s down to 21 KiB/s
and left CPU where it was. It still brackets every frame in DECSC/DECRC even
when nothing is written, because a failing write is how it learns the terminal
is gone.

Numbers move with your config. More fastfetch modules move the greeting, a
wider logo moves the write rate. Reproduce them with your own setup before
trusting them.

