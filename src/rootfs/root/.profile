# ~/.profile — read by a *login* shell, which is what login(1) execs.
#
# bash reads ~/.bashrc for interactive non-login shells and this file for
# login shells, and never both.  Since every shell on this system now arrives
# through getty -> login -> `-bash`, without this line the prompt, aliases and
# PATH in ~/.bashrc would simply never be applied.
if [ -r "$HOME/.bashrc" ]; then
    . "$HOME/.bashrc"
fi

# After a console login, bring up the desktop.  /usr/rc normally starts the
# whole session at boot (its X section runs /sbin/start-xfce); this is the
# fallback for a boot where Xorg is up but the session never came up.
if [ -x /sbin/start-xfce ] && [ ! -f /tmp/run/session-bus ]; then
    DISPLAY=:0 /sbin/start-xfce
fi
