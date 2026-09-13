# ffanim

Animated fastfetch, pinned above a working shell.

![ffanim pinned above a shell, a lit band sweeping down the logo](demo.gif)

fastfetch prints once and exits. `ffanim` keeps that block on screen with a lit
band sweeping down the logo, while your prompt stays a normal prompt underneath:
history, completion, aliases, all of it. It detects nothing itself, so whatever
your `config.jsonc` already prints is what shows up beside the logo.

## Install

Linux, fastfetch, a C compiler. No libraries beyond libc and libm.

    make
    make install

`ffanim` goes to `~/.local/bin` and an example logo to
`~/.local/share/ffanim/`, both under `PREFIX=`. Nothing is written to your
config. Try it:

    ffanim --once

## Pin it to your shell

`~/.config/fish/config.fish`:

    function fish_greeting
        type -q ffanim; or return
        fastfetch --logo none --pipe false | ffanim --pin --stdin
    end

bash or zsh:

    [[ $- == *i* ]] && command -v ffanim >/dev/null && \
        fastfetch --logo none --pipe false | ffanim --pin --stdin

Pipe fastfetch in rather than letting ffanim run it. fastfetch names your shell
by walking the parent process chain, and from inside ffanim that chain leads to
ffanim.

## Options

| | |
|---|---|
| `--pin [--stdin]` | pin above the shell and keep animating |
| `--unpin` | stop it and release the scroll region |
| `--once` | print one static frame and exit |
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

## License

MIT
