<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="doc/images/readme/logo-dark.svg">
    <img alt="OrcaSlicer MCP" src="doc/images/readme/logo-light.svg" width="150">
  </picture>
</p>

<h3 align="center">OrcaSlicer 2.5, with a way in for an AI assistant.</h3>

<p align="center">
  <a href="https://github.com/MaxEllis/OrcaSlicer/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/MaxEllis/OrcaSlicer?label=release&color=009789"></a>
  <a href="https://github.com/OrcaSlicer/OrcaSlicer/releases/tag/v2.5.0"><img alt="Based on OrcaSlicer 2.5.0" src="https://img.shields.io/badge/based%20on-OrcaSlicer%202.5.0-555"></a>
  <a href="LICENSE.txt"><img alt="License: AGPL-3.0" src="https://img.shields.io/badge/license-AGPL--3.0-555"></a>
</p>

<p align="center"><img src="doc/images/readme/story.png" alt="Three steps: a chat in your AI app above, OrcaSlicer below. 1: you write that you designed these brackets, want them as strong as possible, and ask it to show you the changes it would make; OrcaSlicer shows the bracket standing. 2: the assistant makes three changes and says why: on its side, because standing up the layers run across the arm that takes the load; 4 walls instead of 2, because most of a bracket's strength is in its walls; 4 top and bottom layers instead of 3. OrcaSlicer's MCP panel lists each change with Rotate back or Revert. 3: OrcaSlicer's slices compared: 42 min and 36.2 g as designed, 46 min and 53.8 g stronger; lying flat saves 5 minutes."></p>

This is the **OrcaSlicer MCP build**: [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) 2.5.0 with two additions. A local API lets an AI assistant, which you talk to in your usual AI app, change settings, load and slice models, and read back the result, through the [orcaslicer-mcp](https://github.com/MaxEllis/orcaslicer-mcp) server. A panel beside the 3D view lists every change it makes, with a way to undo each one. Everything else is OrcaSlicer, rebuilt from the upstream release, and it uses your existing printers and presets.

Rather keep stock OrcaSlicer? The [plugin](https://github.com/MaxEllis/orcaslicer-mcp#two-ways-to-run-it) covers settings and presets.

## Install

| Platform | File on the [latest release](https://github.com/MaxEllis/OrcaSlicer/releases/latest) |
|---|---|
| Windows 10/11 | `win64-installer.exe` (Arm: `winarm64-installer.exe`), or the portable `.zip` |
| macOS 10.15 and later | `macos-universal.dmg`. It is signed but not notarized: the first launch needs **System Settings → Privacy & Security → Open Anyway**. |
| Linux | `linux-x64.AppImage` or `linux-aarch64.AppImage`, or the `.flatpak` files |

Then open **Preferences → Remote API**, tick **Enable Remote API**, copy the token, and follow the [setup steps](https://github.com/MaxEllis/orcaslicer-mcp#get-started) to connect your assistant app.

<img src="doc/images/readme/preferences.png" width="520" alt="Preferences, Remote API page with Enable Remote API ticked, the port and an example token">

## The panel

<img align="right" src="doc/images/readme/panel-undo.gif" width="300" alt="Clicking a change in the panel, then Revert: the setting goes back in the sidebar">

- **Every change, grouped** by round of work, with the print time of each slice.
- **Undo any one** with the link beside it: Revert, Rotate back, Put back. An undo never overwrites something changed since.
- **Go back to an earlier point** from an older group. It asks first, and can be undone.
- **Your own edits count.** Change something yourself and the panel shows your value; the assistant is told with its next reply.

Close it with its ✕ and bring it back from **View → OrcaSlicer MCP panel**.

<br clear="right">

## Safety

- **Off until you enable it**, and it listens on this computer only. **Allow LAN access** is a separate box.
- **Every request needs the token.** Regenerate it to cut off every client at once.
- **No scripts, G-code or printer connection changes** until you tick **Allow script, G-code and connection edits**, and **no deleting or replacing project files** until you tick **Allow deleting and replacing project files**.
- **Passwords and API keys** are left out of everything the API returns.

## Updates

The build checks this repository for new releases and offers them in OrcaSlicer's usual update dialog. Releases are named after the OrcaSlicer version plus this fork's number (`v2.5.0-mcp.16`), follow upstream's stable releases only, and are built by GitHub Actions with checksums in `SHA256SUMS.txt`.

## Problems and credits

Report problems on the [orcaslicer-mcp issue tracker](https://github.com/MaxEllis/orcaslicer-mcp/issues). Anything that also happens in stock OrcaSlicer belongs with [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer/issues). The API is documented in the [Remote API reference](docs/remote-api/README.md).

OrcaSlicer is made by the [OrcaSlicer team and contributors](https://github.com/OrcaSlicer/OrcaSlicer/graphs/contributors) and builds on Bambu Studio, PrusaSlicer and Slic3r. This fork is AGPL-3.0, like OrcaSlicer ([LICENSE.txt](LICENSE.txt)). If it saves you time, you can [buy me a coffee](https://buymeacoffee.com/maxellis).
