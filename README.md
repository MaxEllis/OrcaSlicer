<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="doc/images/readme/logo-dark.svg">
    <img alt="OrcaSlicer MCP" src="doc/images/readme/logo-light.svg" width="150">
  </picture>
</p>

<h3 align="center">OrcaSlicer with a local API, so an assistant can work the slicer while you watch.</h3>

<p align="center">
  <a href="https://github.com/MaxEllis/OrcaSlicer/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/MaxEllis/OrcaSlicer?label=release&color=009789"></a>
  <a href="https://github.com/OrcaSlicer/OrcaSlicer/releases/tag/v2.5.0"><img alt="Based on OrcaSlicer 2.5.0" src="https://img.shields.io/badge/based%20on-OrcaSlicer%202.5.0-555"></a>
  <a href="https://github.com/MaxEllis/OrcaSlicer/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/MaxEllis/OrcaSlicer/total?color=555"></a>
  <a href="LICENSE.txt"><img alt="License: AGPL-3.0" src="https://img.shields.io/badge/license-AGPL--3.0-555"></a>
</p>

<p align="center">
  <a href="https://github.com/MaxEllis/OrcaSlicer/releases/latest"><b>Download</b></a> ·
  <a href="https://github.com/MaxEllis/orcaslicer-mcp#quickstart"><b>Connect an assistant</b></a> ·
  <a href="https://github.com/MaxEllis/OrcaSlicer/blob/remote-api-250/docs/remote-api/README.md"><b>API reference</b></a> ·
  <a href="https://github.com/MaxEllis/orcaslicer-mcp/issues"><b>Report a problem</b></a>
</p>

---

This is [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) 2.5.0 with one addition: a Remote API built into the app. Paired with the [orcaslicer-mcp](https://github.com/MaxEllis/orcaslicer-mcp) server, an AI assistant can load your model, choose presets, change settings, slice, and read back what the slice will cost in time and filament. It does that in the OrcaSlicer window you already use, so every change shows up on screen and you can take over at any point.

Everything else is OrcaSlicer: the same slicing engine, printer profiles and interface, rebuilt from the upstream release.

<table>
  <tr>
    <td width="50%"><img src="doc/images/readme/plate-editor.png" alt="The Stanford bunny on the build plate, as the API renders the editor view"></td>
    <td width="50%"><img src="doc/images/readme/plate-preview.png" alt="The same bunny sliced with tree supports, toolpaths coloured by feature"></td>
  </tr>
  <tr>
    <td colspan="2" align="center"><sub>Both images came from the API (<code>GET /plate/render</code>), the way an assistant sees the plate before and after slicing.</sub></td>
  </tr>
</table>

## What an assistant can do with it

| | |
|---|---|
| **Settings** | Read and change any print, filament or printer setting while the app runs, for the whole plate, one object, or a height range of an object. |
| **Presets** | Switch, save, rename and delete presets. |
| **Models** | Load STL, 3MF, OBJ and STEP files. Move, rotate, scale, duplicate, arrange and auto-orient them. |
| **Slicing** | Slice the current plate and get the print time, filament use, a breakdown by feature (walls, infill, supports and the rest) and OrcaSlicer's own warnings. |
| **Seeing the plate** | Render the editor or the sliced preview as an image from any side. |
| **Plates** | Add, select and delete plates, and move objects between them. |
| **Projects** | Save, open and start projects the way the File menu does, without a dialog waiting for a click. |
| **Printers** | Map filaments to the trays of an Elegoo Centauri Carbon 2 CANVAS unit. |
| **Live events** | Follow slicing progress, setting changes and opened projects as they happen, over a WebSocket. |

The orcaslicer-mcp server turns this into tools an assistant can call, adds a reference to every OrcaSlicer setting, checks profiles against your printer's physical limits, and can compare several slices side by side. Its [README](https://github.com/MaxEllis/orcaslicer-mcp#what-it-can-do) has the full list.

## Get started

**1. Install the app.** Download it from the [latest release](https://github.com/MaxEllis/OrcaSlicer/releases/latest):

| Platform | File |
|---|---|
| Windows 10/11, x64 | `win64-installer.exe`, or `win64-portable.zip` |
| Windows 11, Arm | `winarm64-installer.exe`, or `winarm64-portable.zip` |
| macOS 10.15 and later, Apple silicon and Intel | `macos-universal.dmg` |
| Linux, x64 and Arm | `linux-x64.AppImage` / `linux-aarch64.AppImage`, or the `.flatpak` files |

It reads and writes the same profile folder as OrcaSlicer, so your printers, filaments and process presets are already there. On macOS the app is signed but not notarized: the first launch needs **System Settings → Privacy & Security → Open Anyway**.

**2. Turn on the API.** Open **Preferences → Remote API** (Ctrl+P on Windows and Linux; on a Mac, the OrcaSlicer menu), tick **Enable Remote API** and copy the token.

<img src="doc/images/readme/preferences.png" width="560" alt="Preferences, Remote API page with Enable Remote API ticked, the port and an example token">

<sub>The token in the picture is an example. OrcaSlicer generates your own; keep it private.</sub>

**3. Connect an assistant.** Add [orcaslicer-mcp](https://github.com/MaxEllis/orcaslicer-mcp#quickstart) to your MCP client: it runs with `uvx orcaslicer-mcp` and `ORCA_API_TOKEN` set to the token, and its quickstart covers each kind of client. Then ask for something, such as "load bracket.stl, slice it in PETG and tell me how long it takes".

## Safety

The API can change what your printer does, so the defaults are cautious, and loosening them is up to you.

- **Off until you enable it.** It listens on this computer only. Tick **Allow LAN access** only if the MCP server runs on another machine on your network.
- **Every request needs the token.** Regenerating it in Preferences cuts off every client that has the old one.
- **No scripts, G-code or connection changes.** While **Allow script, G-code and connection edits** is off, the API refuses to change post-processing scripts, custom G-code, OrcaSlicer 2.5's Python slicing plugins, and the printer's address and credentials. It also refuses to open a project that brings its own versions of those, and says which settings differ. Text an assistant reads, such as a file name or a web page, cannot become a command on your computer or printer this way.
- **No deleting or replacing files.** While **Allow deleting and replacing project files** is off, the API cannot delete a project file or save or move one over an existing file.
- **Credentials stay out of replies.** API keys, passwords and any `user:password` in a printer address are removed from what the API returns.

The orcaslicer-mcp server applies the same rule about scripts and G-code on its side, so both have to agree before such a change goes through.

## Versions and updates

Releases are named after the OrcaSlicer version they are built on, plus this fork's own release number: `v2.5.0-mcp.16` is OrcaSlicer 2.5.0 with fork release 16. The fork moves to a new OrcaSlicer version once upstream ships it as a stable release, never to a beta.

The app checks this repository for a newer release on startup and offers it in OrcaSlicer's usual update dialog. It does not offer stock OrcaSlicer updates, which would remove the API.

Every release is built by GitHub Actions from this repository for all platforms, and its checksums are in `SHA256SUMS.txt` next to the downloads.

## Problems and questions

Report problems on the [orcaslicer-mcp issue tracker](https://github.com/MaxEllis/orcaslicer-mcp/issues); issues are turned off on this repository. If something also goes wrong in stock OrcaSlicer, it belongs with [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer/issues), whose [wiki](https://github.com/OrcaSlicer/OrcaSlicer/wiki) covers the slicer itself.

## Credits and license

OrcaSlicer is made by the [OrcaSlicer team and contributors](https://github.com/OrcaSlicer/OrcaSlicer/graphs/contributors). It builds on Bambu Studio, PrusaSlicer and Slic3r. This fork is licensed under the GNU Affero General Public License v3, like OrcaSlicer itself; see [LICENSE.txt](LICENSE.txt).

If the fork saves you time and you feel like saying thanks, you can buy me a coffee.

<a href="https://buymeacoffee.com/maxellis"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" height="44"></a>
