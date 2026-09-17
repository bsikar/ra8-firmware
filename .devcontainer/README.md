# Dev container

VS Code Dev Container for ra8-firmware: pinned Ubuntu toolchain, zsh +
oh-my-zsh + powerlevel10k, and the repo's git hooks. Open the folder in VS Code
and pick "Reopen in Container".

## Terminal font (one time, on your host)

The prompt uses powerlevel10k icons, which need a Nerd Font. VS Code draws the
terminal font on your host machine (Windows/macOS), so the container cannot
install it for you. Install it once:

1. Open `.devcontainer/fonts/` and install all four `MesloLGS NF` files
   (on Windows: select all, right-click, Install; on macOS: open each in Font
   Book, Install).
2. Reload the VS Code window.

The terminal is already set to `MesloLGS NF` (see `devcontainer.json`), so once
the font is installed the icons render with no further changes. Without it the
prompt still works, just with boxes where the icons should be.

## Slow on Windows?

A bind mount over the Windows filesystem is slow for git and builds. For a big
speedup, keep the repo in a container volume: run "Dev Containers: Clone
Repository in Container Volume" instead of opening a bind-mounted folder.
