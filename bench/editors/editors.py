"""Per-editor command lines, clean configs, and keystrokes for run.py.

Every editor runs with a throwaway $HOME / XDG dirs (created by run.py) so
no user config leaks in. Syntax highlighting is ON wherever the editor has
it by default or via its stock syntax files; `highlight` documents how.

Keystroke fields are raw bytes written to the pty. A "script" is a list of
steps; each step is either bytes (send) or ("wait", regex, timeout_s) which
waits for the regex on the virtual screen before the next step (used for
confirmation prompts such as "discard changes?").

Paths can be overridden with env vars (CCTEXT_BIN, NVIM_BIN, HX_BIN, ...);
the defaults match the Dockerfile layout (/opt/editors) with a PATH fallback.
"""
import os
import re
import shutil

ESC = b"\x1b"
CR = b"\r"
PGDN = b"\x1b[6~"
DOWN = b"\x1b[B"


def ctrl(c):
    return bytes([ord(c.lower()) & 0x1F])


def _bin(env, *cands):
    v = os.environ.get(env)
    if v:
        return v
    for c in cands:
        if os.path.isabs(c):
            if os.path.exists(c):
                return c
        else:
            p = shutil.which(c)
            if p:
                return p
    return cands[-1]


HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


class Editor:
    name = "?"
    bin = None
    highlight = ""
    goto_kind = "line"      # "pct" (byte or line percent) or "line"
    notes = ""
    # auto-answers for prompts that can appear while opening: (regex, bytes)
    open_prompts = []

    def setup(self, home):
        """Write clean config files under the throwaway HOME."""

    def argv(self, path, home):
        raise NotImplementedError

    def version_argv(self):
        return [self.bin, "--version"]

    def env(self, home):
        return {}

    # -- keystrokes ------------------------------------------------------
    # Modal editors are in normal mode whenever these run, so no leading
    # ESC: a busy editor would read "ESC :" as Alt-: (seen in helix).
    # goto()/search() return (setup_steps, timed_steps): the clock starts
    # when the first timed step is written and stops when the screen shows
    # the expected content. Incremental searches (emacs, micro, cctext)
    # therefore include search-as-you-type work in the timed span.
    def goto(self, line, pct):
        raise NotImplementedError

    def search(self, pat):
        raise NotImplementedError

    insert_enter = b""
    insert_exit = b""
    pagedown = PGDN

    def content(self, disp):
        """Screen rows that belong to the document view (used to decide
        that a search has landed rather than merely listed a hit)."""
        return disp

    def quit_discard(self):
        raise NotImplementedError


class CCText(Editor):
    name = "cctext"
    highlight = "built-in window lexer / TextMate grammars (shipped grammars/ next to binary)"
    goto_kind = "pct"
    notes = ("`--no-blink` so the caret does not repaint on a timer; Ctrl-G `50%` is a byte-percent jump. "
             "No PageDown key: scroll uses mouse-wheel events. Find is incremental; Down lands on the hit.")

    def __init__(self):
        self.bin = _bin("CCTEXT_BIN", os.path.join(REPO, "bin", "cctext"), "/opt/editors/cctext/cctext", "cctext")

    def version_argv(self):
        return [self.bin, "--version"]

    def env(self, home):
        g = os.environ.get("RTX_GRAMMARS") or os.path.join(REPO, "testdata", "grammars")
        return {"RTX_GRAMMARS": g} if os.path.isdir(g) else {}

    def argv(self, path, home):
        return [self.bin, "--no-blink", path]

    def goto(self, line, pct):
        return [ctrl("g")], [b"50%" + CR]

    def search(self, pat):
        # find is incremental; Down lands on the next hit once the scan has
        # listed it (the hit list rows start with a line number).
        hit = r"(?m)^ *\d+ .*" + re.escape(pat)
        return [ctrl("f")], [pat.encode(), ("wait", hit, None), DOWN]

    # No PageDown binding in the TUI: one "page" is 24 SGR wheel-down events
    # (2 rows each ~= one 48-row screen) written in a single burst.
    pagedown = b"\x1b[<65;100;20M" * 24

    def content(self, disp):
        for i, row in enumerate(disp):
            if row.startswith(" find:"):
                return disp[:i]
        return disp

    def quit_discard(self):
        return [ctrl("q"), ("wait", r"don.t save", 5), b"q"]


class Vim(Editor):
    name = "vim"
    highlight = "`syntax on` + `filetype plugin indent on` (stock runtime; 'redrawtime' default 2000ms may auto-disable)"
    goto_kind = "pct"
    notes = "vim -u <vimrc> -n -i NONE (no swap, no viminfo)."

    def __init__(self):
        self.bin = _bin("VIM_BIN", "vim")

    def setup(self, home):
        with open(os.path.join(home, "vimrc"), "w") as f:
            f.write("set nocompatible\nsyntax on\nfiletype plugin indent on\nset laststatus=2 ruler\n")

    def argv(self, path, home):
        return [self.bin, "-u", os.path.join(home, "vimrc"), "-n", "-i", "NONE", path]

    def goto(self, line, pct):
        return [], [b"50%"]

    def search(self, pat):
        return [b"/"], [pat.encode() + CR]

    insert_enter = b"i"
    insert_exit = ESC

    def quit_discard(self):
        return [b":qa!" + CR]


class Neovim(Vim):
    name = "nvim"
    highlight = "--clean defaults (syntax on, regex syntax files; bundled treesitter not auto-started for C/JSON)"
    notes = "nvim --clean -n -i NONE. TUI + embedded server are two processes; RSS is the sum of the tree."

    def __init__(self):
        self.bin = _bin("NVIM_BIN", "/opt/editors/nvim-linux-x86_64/bin/nvim", "nvim")

    def setup(self, home):
        pass

    def argv(self, path, home):
        return [self.bin, "--clean", "-n", "-i", "NONE", path]


class Helix(Editor):
    name = "helix"
    highlight = "default theme + bundled tree-sitter grammars (runtime/ from the release tarball)"
    notes = "hx -c <empty config.toml>. No percent goto: `:goto N`."

    def __init__(self):
        self.bin = _bin("HX_BIN", "/opt/editors/helix/hx", "/opt/editors/helix-25.07.1-x86_64-linux/hx", "hx")

    def setup(self, home):
        open(os.path.join(home, "hx.toml"), "w").close()

    def argv(self, path, home):
        return [self.bin, "-c", os.path.join(home, "hx.toml"), path]

    def goto(self, line, pct):
        return [b":goto %d" % line], [CR]

    def search(self, pat):
        return [b"/"], [pat.encode() + CR]

    insert_enter = b"i"
    insert_exit = ESC

    def quit_discard(self):
        return [b":qa!" + CR]


class Kakoune(Editor):
    name = "kakoune"
    highlight = "stock kakrc autoload (filetype highlighters on)"
    notes = "KAKOUNE_CONFIG_DIR=<empty>; system kakrc loads. `Ng` goes to line N."

    def __init__(self):
        self.bin = _bin("KAK_BIN", "/opt/editors/kak/bin/kak", "kak")

    def env(self, home):
        d = os.path.join(home, "kakcfg")
        os.makedirs(d, exist_ok=True)
        return {"KAKOUNE_CONFIG_DIR": d}

    def version_argv(self):
        return [self.bin, "-version"]

    def argv(self, path, home):
        return [self.bin, path]

    def goto(self, line, pct):
        return [], [b"%dg" % line]

    def search(self, pat):
        return [b"/"], [pat.encode() + CR]

    insert_enter = b"i"
    insert_exit = ESC

    def quit_discard(self):
        return [b":q!" + CR]


class Micro(Editor):
    name = "micro"
    highlight = "default colorscheme + built-in syntax files"
    notes = "micro -config-dir <empty>. Goto via command bar `goto N`."

    def __init__(self):
        self.bin = _bin("MICRO_BIN", "/opt/editors/micro/micro", "/opt/editors/micro-2.0.15/micro", "micro")

    def argv(self, path, home):
        d = os.path.join(home, "microcfg")
        os.makedirs(d, exist_ok=True)
        return [self.bin, "-config-dir", d, path]

    def goto(self, line, pct):
        return [ctrl("e"), b"goto %d" % line], [CR]

    def search(self, pat):
        return [ctrl("f")], [pat.encode() + CR]

    def quit_discard(self):
        return [ctrl("q"), ("wait", r"(?i)save changes", 5), b"n"]


class Nano(Editor):
    name = "nano"
    highlight = "--rcfile with `include /usr/share/nano/*.nanorc`"
    notes = "Goto line via Ctrl-_ ."

    def __init__(self):
        self.bin = _bin("NANO_BIN", "nano")

    def setup(self, home):
        with open(os.path.join(home, "nanorc"), "w") as f:
            f.write('include "/usr/share/nano/*.nanorc"\n')

    def argv(self, path, home):
        return [self.bin, "--rcfile", os.path.join(home, "nanorc"), path]

    def goto(self, line, pct):
        return [ctrl("_"), b"%d" % line], [CR]

    def search(self, pat):
        return [ctrl("w")], [pat.encode() + CR]

    def quit_discard(self):
        return [ctrl("x"), ("wait", r"(?i)save modified", 5), b"n"]


class Emacs(Editor):
    name = "emacs"
    highlight = "emacs -Q defaults (global-font-lock-mode on; js-json-mode / c-mode)"
    notes = ("emacs -nw -Q, lockfiles/backups/auto-save off. Files > 10 MB ask "
             "'really open?' — answered y (keeps major mode + font-lock). Goto line via M-g g.")
    open_prompts = [(r"really open\?", b"y")]

    def __init__(self):
        self.bin = _bin("EMACS_BIN", "emacs")

    def argv(self, path, home):
        return [self.bin, "-nw", "-Q", "--eval",
                "(setq create-lockfiles nil make-backup-files nil auto-save-default nil"
                " inhibit-startup-screen t)", path]

    def goto(self, line, pct):
        return [ctrl("g"), ESC + b"gg", ("wait", r"Goto line", 5), b"%d" % line], [CR]

    def search(self, pat):
        return [ctrl("g"), ctrl("s")], [pat.encode() + CR]

    def quit_discard(self):
        return [ctrl("g"), ctrl("x") + ctrl("c"), ("wait", r"Save file", 5), b"n",
                ("wait", r"exit anyway", 5), b"yes" + CR]


ALL = [CCText, Vim, Neovim, Helix, Kakoune, Micro, Nano, Emacs]


def get(names=None):
    eds = [c() for c in ALL]
    if names:
        want = [n.strip() for n in names.split(",") if n.strip()]
        eds = [e for e in eds if e.name in want]
    return eds
