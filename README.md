# Milanote++

A small Windows program that puts your **Milanote** boards inside **Claude**.

It is a single `Milanote++.exe` that does three things:

1. **Milanote login** – opens Milanote's own sign‑in page in a private browser profile (WebView2) and keeps that session on your machine. Your Milanote password never passes through Milanote++ or Claude.
2. **MCP server** – when Claude Desktop or Claude Code start `Milanote++.exe --mcp`, it speaks the [Model Context Protocol](https://modelcontextprotocol.io) over stdio and exposes 19 tools (`milanote_build_board`, `milanote_get_board`, `milanote_export_board`, `milanote_search`…), 11 prompts, 8 board templates and a built‑in guide. All of it is compiled into the exe — nothing to configure.
3. **Claude wiring** – a one‑click "Install into Claude Desktop" button (writes `claude_desktop_config.json`, both the Store‑package and the classic location) and a copy‑paste `claude mcp add …` command for Claude Code.

You log in to Claude in the Claude app as usual and use your normal Claude subscription there; Milanote++ never asks for or stores Claude credentials. (Anthropic does not allow third‑party apps to sign in with Claude.ai accounts — registering a local MCP server is the supported way to give Claude access to another product.)

> **Unofficial.** Milanote has no public API. Milanote++ talks to the same private web API the Milanote web app uses (see [`docs/milanote-api.md`](docs/milanote-api.md)). That API can change without notice, and automating Milanote this way may not be covered by Milanote's terms of service. Milanote++ is not affiliated with Milanote or Anthropic. Use at your own risk.

## Build

Requirements: Visual Studio 2026 (or 2022) with the *Desktop development with C++* workload, Windows 10/11, and the [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (already present on any Windows with Edge).

1. Open `Milanote++.slnx`.
2. Restore NuGet packages (right‑click the solution → *Restore NuGet Packages*; VS usually does this automatically on the first build). The only package is `Microsoft.Web.WebView2`; the loader is linked statically, so the output is just `Milanote++.exe`.
3. Build **Release | x64**.

`bridge.js`, `setup.html`, `overlay.js`, `blackout.js`, `skill.md` (the guide) and `prompts.json` (prompts + templates) are embedded into the exe as resources (`RCDATA`), and `nlohmann/json` is vendored under `Milanote++/third_party`.

## Use

1. Run `Milanote++.exe`. In the setup window click **Sign in to Milanote** and log in (Google/Apple/email all work — it is the real Milanote login page). The window returns to the setup page once the session is detected.
2. Click **Install into Claude Desktop**, then restart Claude Desktop (quit it from the tray icon and open it again). The Milanote tools appear in the tools menu of a new chat. Milanote++ updates every config Claude Desktop might read: the Microsoft Store/MSIX install keeps its own copy under `%LOCALAPPDATA%\Packages\Claude_<id>\LocalCache\Roaming\Claude\`, the classic installer uses `%APPDATA%\Claude\claude_desktop_config.json`.
   For Claude Code, click **Copy command** and run it in a terminal (`claude mcp add -s user milanote -- "…\Milanote++.exe" --mcp`).
3. **Test connection** reads your board tree through the exact path Claude uses.

Command line: `--login`, `--mcp`, `--install`, `--uninstall`, `--print-config`, `--whoami`, `--update`, `--rollback`, `--version`, `--help`.

While Claude Desktop is not running, the Milanote++ window is blacked out with a prompt to open it (or a download link when it is not installed); the blackout lifts by itself when Claude Desktop appears and returns if it is closed again. Claude Code users can dismiss it for the session.

Everything Milanote++ stores lives in `%LOCALAPPDATA%\Milanote++` (the WebView2 profile with the Milanote session, and `milanote++.log`). Delete that folder to sign out completely.

## What Claude gets

Everything below is built into the exe and available the moment Claude connects — no configuration, no files to copy.

### Skill (server instructions)

On connect the server hands Claude [`Milanote++/skill.md`](Milanote++/skill.md): the workflow (whoami → find → build/edit → link), the outline format, formatting rules, positions/ids, plan limits and etiquette (never trash unasked, keep Home tidy). It is also readable as the `milanote://guide` resource.

### Tools

| Tool | What it does |
| --- | --- |
| `milanote_whoami` | Signed‑in user, Home board id, quick‑notes board, plan usage |
| `milanote_login` | Opens the sign‑in window when a session is missing |
| `milanote_list_boards` | Board tree with ids, paths, URLs and element counts |
| `milanote_get_board` | Readable dump of a board: notes (Markdown‑ish), columns with children, to‑do lists with done state, links, images, sub‑boards, all with ids; `raw` for Milanote's JSON |
| `milanote_get_element` | One element by id (with its board id) |
| **`milanote_build_board`** | **Builds a whole board in one call** from a Markdown outline (or a `spec` object): sub‑board, columns, notes, to‑do lists, link cards |
| **`milanote_export_board`** | A board as a Markdown outline (same format), optionally with element ids — the way to read a board for summaries and rewrites |
| **`milanote_templates`** / **`milanote_apply_template`** | List / fetch the built‑in board templates; create an empty skeleton from one |
| `milanote_create_board` / `milanote_create_column` | New sub‑board / column on a board |
| `milanote_create_note` | Note on a board or inside a column; light Markdown (`#`, `-`, `- [ ]`, `>`, `---`, `**bold**`, `*italic*`, `` `code` ``, URLs); line breaks are kept |
| `milanote_create_todo_list` / `milanote_add_task` | To‑do lists and tasks (strings or `{text, done}`) |
| `milanote_create_link` | Web link card with Milanote's title/preview |
| `milanote_update_element` | Change text, title, done state, url or colour |
| `milanote_move_element` | Move to a board canvas, into a column, or into another to‑do list |
| `milanote_trash_element` | Move to the board trash (restorable in Milanote) |
| `milanote_search` | Text search across boards |

New canvas elements are placed to the right of existing content (positions are in Milanote grid units, ≈ 9 px); pass `x`/`y` or `index` to choose a spot.

### Outline format

`milanote_build_board` and `milanote_export_board` share one plain‑text format, so a board can be written, read back and rewritten as a document:

```
# Board title                 ← creates a new sub-board; plain text under it becomes the title note
## Column title               ← a column
### Note heading              ← a note; runs until the next ### / ##, light Markdown inside
### To-do list title          ← a section made only of "- [ ] task" / "- [x] done" lines
- [ ] first task
###                           ← bare ### = a note or link without a heading
https://example.com Caption   ← a section that is only a URL (+ caption) becomes a link card with a preview
```

[`docs/examples/tic-tac-toe-gdd.md`](docs/examples/tic-tac-toe-gdd.md) is a complete example (a game design document).

### Prompts (Claude Desktop "+" menu, `/mcp__milanote__…` in Claude Code)

| Prompt | Arguments | Result |
| --- | --- | --- |
| `gdd` | game, platform | A complete game design document board |
| `character_sheet` | character, game | Identity, visuals, role & stats, moveset, animations, tasks |
| `level_design` | level, game | Purpose, layout & flow, encounters, art & audio, blockout tasks, playtest checklist |
| `brainstorm` | topic | Ideas, questions, constraints, wild cards, next steps |
| `moodboard` | theme, references | Direction, colour & light, shapes & materials, reference cards, do/don't |
| `project_plan` | project, deadline | Goal & scope, milestones, backlog, this week, risks & decisions |
| `meeting_notes` | meeting, notes | Raw notes organised into agenda, discussion, decisions, actions, follow‑ups |
| `weekly_plan` | focus, week | Top outcomes plus a to‑do list per weekday |
| `summarize_board` | board | Structured summary of an existing board with next steps |
| `organize_board` | board | Proposes columns for loose notes, applies them after approval |
| `notes_to_tasks` | board | Extracts the actions from a board into a to‑do list |

The building prompts embed the matching template and tell Claude to write real content in that structure and create it with a single `milanote_build_board` call.

### Resources

`milanote://guide` (the skill), `milanote://templates` (all templates as outlines), `milanote://boards` (live board tree, JSON) and `milanote://board/{boardId}` (any board as an outline).

Prompts, templates and snippets live in [`Milanote++/prompts.json`](Milanote++/prompts.json) (`{{argument}}`, `{{template:name}}` and `{{snippet:name}}` placeholders); the guide is [`Milanote++/skill.md`](Milanote++/skill.md). Edit them and rebuild — no code changes needed.

## Updates

Milanote++ updates itself from this repository's [releases](https://github.com/xxNightshade/milanote-plus-plus/releases). The contract is fixed so that every version, now and later, can update to the next one:

* a release is a tag `vX.Y.Z` with two assets, `Milanote++.exe` and `Milanote++.exe.sha256` (the hex digest);
* the running exe asks the GitHub API for the latest release when it starts (the setup window at once, the MCP server 20 s after Claude starts it; at most once every 6 hours), compares the tag with its own version, downloads the exe, verifies the SHA‑256, and swaps files: the running exe is renamed to `Milanote++.old.exe` and the new one copied into its place. It is used the next time Milanote++ starts — and the next time Claude Desktop starts the server;
* the old version does the whole swap, so a release can never break the mechanism for the versions already installed; the previous exe stays for a week as a rollback (`--rollback` or the setup page);
* the registered path in Claude's config never changes.

Publishing a release: bump `kVersion` in `Milanote++/Util.h`, build Release x64, compute the digest (`certutil -hashfile Milanote++.exe SHA256` → `Milanote++.exe.sha256`), tag `vX.Y.Z`, and create the GitHub release with both files attached.

## How it works

```
Claude Desktop / Claude Code
        │  stdio (JSON-RPC / MCP)
        ▼
Milanote++.exe --mcp          C++ (Win32, WRL, nlohmann/json)
        │  window.chrome.webview messages
        ▼
hidden WebView2 on https://app.milanote.com/api/ping   ← bridge.js
        │  fetch (/api/*) + socket.io "action" events, with the user's session cookie
        ▼
Milanote
```

* `WebPane.*` hosts WebView2 and shuttles JSON between C++ and the page.
* `bridge.js` is the actual Milanote client: REST reads, a minimal Socket.IO/Engine.IO v4 client for writes, id generation, TipTap ⇄ Markdown conversion, and the tool implementations.
* `McpServer.*` is the stdio MCP server (initialize, tools, prompts, resources); `skill.md` and `prompts.json` are the catalogue it serves.
* `SetupApp.*` + `setup.html` is the setup window; `Installer.*` edits Claude Desktop's config.
* `ClaudeDesktop.*` finds, watches and launches Claude Desktop; `blackout.js` draws the blackout; `ServerLock.h` is the "a server is running" signal between the processes.
* `Updater.*` is the GitHub Releases self-update (WinHTTP + BCrypt SHA‑256).

## Known limitations

* Free Milanote accounts are limited to 100 notes/tasks/links; `milanote_whoami` reports the usage and the guide tells Claude to check it, but Milanote enforces the limit.
* Images/files cannot be uploaded yet (only read); lines, comments, sketches and tables are read‑only.
* If a board created by Claude looks empty in an already‑open Milanote tab, reload the tab (Milanote caches boards locally).
