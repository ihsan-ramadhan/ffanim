# ffanim

Animated fastfetch, pinned above a working shell.

![ffanim pinned above a shell, a lit band sweeping down the logo](demo.gif)

fastfetch prints once and exits. `ffanim` keeps that block on screen with a lit
band sweeping down the logo, while your prompt stays a normal prompt underneath:
history, completion, aliases, all of it. It detects nothing itself, so whatever
your `config.jsonc` already prints is what shows up beside the logo.

## Install

    curl -fsSL https://raw.githubusercontent.com/ihsan-ramadhan/ffanim/main/install.sh | sh

Linux, fastfetch, a C compiler. No libraries beyond libc and libm. The script
fetches this repo, builds it, and leaves `ffanim` in `~/.local/bin` with an
example logo in `~/.local/share/ffanim/`. Set `PREFIX=/usr/local` to move both.
Nothing is written to your config.

From a clone it is the same two steps the script runs:

    make
    make install

See it move, then press Ctrl-C:

    ffanim

## Put it in your shell

Two ways to run it, and they are a real choice rather than two spellings of the
same thing.

**Pinned above your prompt.** `~/.config/fish/config.fish`:

    function fish_greeting
        type -q ffanim; or return
        fastfetch --logo none --pipe false | ffanim --pin --stdin
    end

bash or zsh:

    [[ $- == *i* ]] && command -v ffanim >/dev/null && \
        fastfetch --logo none --pipe false | ffanim --pin --stdin

The block is painted onto the top rows of the live screen every frame, so it
never enters the terminal's buffer. It holds its place no matter how much you
print, and scrolling up will not find it in your history, because it was never
written there.

**Scrolling away like ordinary output.** `~/.config/fish/config.fish`:

    if status is-interactive; and not set -q FFANIM_WRAPPED; and type -q ffanim
        fastfetch --logo none --pipe false | ffanim --wrap --stdin
        set -l rc $status
        exec sh -c "exit $rc"
    end

bash or zsh:

    if [[ $- == *i* && -z $FFANIM_WRAPPED ]]; then
        fastfetch --logo none --pipe false | ffanim --wrap --stdin
        exec sh -c "exit $?"
    fi

ffanim prints the block, starts your shell on a pty of its own, and relays
between that pty and the terminal. Every byte passes through it, so it knows the
moment the screen scrolls, and that is when it stops for good and becomes a
plain relay. The block it printed is then ordinary scrollback, exactly as
printed, and it rides up and out like any other output. Full-screen programs are
left alone too: while one owns the screen ffanim paints nothing, and picks up
again when it exits.

What that costs is not speed. Measured here the relay adds 1.8 us per keystroke,
against the 5 to 20 ms of input latency you already have, and carries 123 MB/s
where a bare pty carries 155 MB/s, far past what a terminal can draw. What it
costs is that your shell's parent is now ffanim, so if ffanim dies the session
goes with it.

Either way, pipe fastfetch in rather than letting ffanim run it. fastfetch names
your shell by walking the parent process chain, and from inside ffanim that
chain leads to ffanim.

`--once` is not a third way to run it. It prints one frame and exits, which is
what a script wants, and the only mode that works with no terminal at all.

## Options

| | |
|---|---|
| `--pin [--stdin]` | pin above the shell and keep animating |
| `--unpin` | stop it and release the scroll region |
| `--off`, `--on` | stop animating, start again |
| `--uninstall` | delete ffanim and the logo it installed |
| `--once` | print one static frame and exit |
| `--wrap [--stdin]` | run your shell inside ffanim, see below |
| `--fps <n>` | frames per second, default 20 |
| `--step <n>` | rows the band moves per frame, default 0.35 |
| `--refresh <n>` | re-read the info pane every n seconds, default off |
| `FFANIM_LOGO` | logo file; see the search order below |
| `FFANIM_CONFIG` | fastfetch config, default is whatever `fastfetch` uses |
| `FFANIM_PROMPT_LEAD` | rows your prompt sits below the cursor, default 2 |

A shell alias for `fastfetch` is invisible to ffanim, which spawns fastfetch
directly rather than through a shell. If you have one, put the same config in
`FFANIM_CONFIG` so both agree.

The logo is a plain text file, one line per row. Braille art (U+28xx) is what
this is built for, because braille cells decode back into a dot bitmap and can
be rescaled when the terminal is small. Any image-to-braille converter makes
one.

ffanim takes the first of these it can read, so dropping a file in the second
one is all it takes to use your own:

    $FFANIM_LOGO
    ~/.config/ffanim/logo
    ~/.config/fastfetch/logo_braille
    $PREFIX/share/ffanim/logo_braille

## What to expect

Full-screen apps take the screen and the painter stays out of their way. vim,
less, htop, a coding agent: it stops writing entirely until they exit, then
repaints. Ordinary commands keep animating right through.

Resizing re-pins the block, which clears the visible screen. The scrollback is
left alone, so what scrolled off is still there, but the screenful you were
looking at goes.

A terminal too small for the block shrinks it, dropping blank lines and
rescaling braille art. Smaller still and `--pin` falls back to plain fastfetch.

The info pane is a snapshot of the moment you opened the terminal, so uptime
and memory go stale. `--refresh 30` keeps them current by re-reading fastfetch
on that interval and replacing only the lines that actually move, leaving the
rest as your shell produced them.

It costs about 2.4 ms on top of the fastfetch you were already running, then
1.6 MB, 0.22 % of one core and 21 KiB/s to the terminal while it animates at
20 fps.

[NOTES.md](NOTES.md) has the reasoning behind each of those, and the
benchmarks. `make test` builds a second binary that pins a painter on a
throwaway pty and checks all of it.

## Turn it off

    ffanim --off
    ffanim --on

Off leaves the snippet in your shell config where it is. The block still prints,
once, and nothing animates: under `--wrap` ffanim hands the terminal straight to
your shell and is gone, under `--pin` it prints and exits. The switch is one
empty file at `~/.config/ffanim/off`, so a second terminal picks it up too.

## Uninstall

    ffanim --uninstall

It deletes itself and the example logo, then names any shell config that still
starts ffanim. Delete that block and your greeting goes back to what it was
before, usually plain fastfetch. From a clone, `make uninstall` reaches the same
two files.

## License

MIT
