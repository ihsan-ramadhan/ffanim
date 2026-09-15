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
fetches this repo, builds it, leaves `ffanim` in `~/.local/bin` with an example
logo in `~/.local/share/ffanim/`, and starts it from your shell config. Open a
new terminal and it is there.

The shell part is one marked block appended to `config.fish`, `.zshrc` or
`.bashrc`, whichever matches your `$SHELL`, and your old file is copied to
`<file>.ffanim.bak` first. `ffanim --unsetup` takes the block back out,
`ffanim --setup --pin` writes the pinned mode instead, and `FFANIM_NO_SETUP=1`
on the install line skips the step entirely. Set `PREFIX=/usr/local` to move the
files. Fish completions are installed alongside.

`ffanim --status` answers the question you will have later:

    ffanim 0.1.1
      animation  glitch
      colour     255,255,255 (default)
      animating  yes
      shell      started from /home/you/.config/fish/config.fish
      logo       /home/you/.config/fastfetch/logo_braille
      fastfetch  its own default config

## Animations

| `sweep`, the default | `wave` |
|:--:|:--:|
| <img src="docs/sweep.gif" width="240" alt="one lit band travelling down the logo"> | <img src="docs/wave.gif" width="240" alt="several lit bands travelling down the logo at once"> |
| one band down the logo, then again from the top | several bands at once, never fully dark |
| **`pulse`** | **`bounce`** |
| <img src="docs/pulse.gif" width="240" alt="the whole logo brightening and dimming together"> | <img src="docs/bounce.gif" width="240" alt="a lit band turning around at the bottom of the logo"> |
| the whole logo brightening and dimming together | a sweep that turns around instead of starting over |
| **`glitch`** | **`ripple`** |
| <img src="docs/glitch.gif" width="240" alt="a band where the logo falls apart into loose dots and reassembles"> | <img src="docs/ripple.gif" width="240" alt="the logo bending from side to side as a wave runs down it"> |
| dots knocked out by a passing band, then put back | rows slid sideways by a wave running down |

Pick one and it stays picked:

    ffanim --anim wave
    ffanim --color 120,200,255

Both save and exit, so the snippet in your shell config never has to change and
every terminal after that picks the new one up. The choice lives in
`~/.config/ffanim/anim` and `~/.config/ffanim/color`, one word per file, and a
file you damage is ignored rather than fatal. `--color` is the colour the logo
reaches at full brightness, so `120,200,255` is a blue logo dimming to near
black. `--fps` and `--step` set the speed of all six.

They do not all cost the same. The painter only sends rows that changed, so the
ones with a travelling band are cheap and the ones that touch every row are not.
On a 16 row logo at 20 fps: glitch 10 KiB/s, sweep 14, ripple 32, wave and pulse
42. All of it is far below anything you would notice.

<details>
<summary><b>The two ways to run it</b></summary>

They are a real choice rather than two spellings of the same thing. `--setup`
writes the second one.

**Pinned above your prompt.** Not what `--setup` writes, so this one is by hand:
run `ffanim --unsetup` first, then in `~/.config/fish/config.fish`:

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

**Scrolling away like ordinary output.** This is what `--setup` writes, so there
is nothing to paste.

ffanim prints the block, starts your shell on a pty of its own, and relays
between that pty and the terminal. Every byte passes through it, so it knows the
moment the screen scrolls, and that is when it stops for good and becomes a
plain relay. The block it printed is then ordinary scrollback, exactly as
printed, and it rides up and out like any other output. Full-screen programs are
left alone too: while one owns the screen ffanim paints nothing, and picks up
again when it exits.

What that costs is not speed. Measured here the relay adds 1.8 us per keystroke,
against the 5 to 20 ms of input latency you already have, and carries 108 MB/s
where a bare pty carries 115 MB/s, far past what a terminal can draw. What it
costs is that your shell's parent is now ffanim, so if ffanim dies the session
goes with it.

Either way, pipe fastfetch in rather than letting ffanim run it. fastfetch names
your shell by walking the parent process chain, and from inside ffanim that
chain leads to ffanim.

`--once` is not a third way to run it. It prints one frame and exits, which is
what a script wants, and the only mode that works with no terminal at all.

</details>

<details>
<summary><b>Options and logos</b></summary>

| | |
|---|---|
| `--pin [--stdin]` | pin above the shell and keep animating |
| `--unpin` | stop it and release the scroll region |
| `--wrap [--stdin]` | run your shell inside ffanim |
| `--once` | print one static frame and exit |
| `--anim <name>` | save the animation: sweep, wave, pulse, bounce, glitch, ripple |
| `--color <r,g,b>` | save the logo colour at full brightness, default 255,255,255 |
| `--off`, `--on` | stop animating, start again |
| `--setup [--pin]` | start ffanim from your shell config, in a marked block |
| `--unsetup` | take that block back out |
| `--status` | what is set, and which file starts it |
| `--uninstall` | delete ffanim and the logo it installed |
| `--version` | print the version and exit |
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

</details>

<details>
<summary><b>What to expect</b></summary>

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

It costs about a millisecond on top of the fastfetch you were already running,
more with a large logo, then 1.6 MB, 0.22 % of one core and 21 KiB/s to the
terminal while it animates at 20 fps.

[NOTES.md](NOTES.md) has the reasoning behind each of those, and the
benchmarks. `make test` builds a second binary that pins a painter on a
throwaway pty and checks all of it.

</details>

<details>
<summary><b>Turn it off, or remove it</b></summary>

    ffanim --off
    ffanim --on

Off leaves the snippet in your shell config where it is. The block still prints,
once, and nothing animates: under `--wrap` ffanim hands the terminal straight to
your shell and is gone, under `--pin` it prints and exits. The switch is one
empty file at `~/.config/ffanim/off`, so a second terminal picks it up too.

    ffanim --uninstall

That takes the marked block out of your shell config, deletes ffanim itself and
the example logo, and names any other file that still mentions ffanim, for
setups written by hand. Your greeting goes back to what it was before, usually
plain fastfetch. From a clone, `make uninstall` reaches the two files but leaves
your shell config alone.

</details>

## Credits

ffanim probes nothing. The OS, kernel, uptime and everything else in that pane
is [fastfetch](https://github.com/fastfetch-cli/fastfetch) output, read straight
from its stdout and reprinted beside the animated logo.

## License

MIT
