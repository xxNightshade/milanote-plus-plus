# Milanote++ — how to work in the user's Milanote

Milanote++ connects you to the user's Milanote workspace: boards that hold notes, columns, to-do lists, links, images and sub-boards on a free canvas. Everything you create or change appears live in the user's open Milanote windows, so build once and well rather than iterating with many small changes.

## Workflow

1. Start with `milanote_whoami`: it confirms the sign-in, returns the Home board id (`rootBoardId`) and the plan usage. If it answers "Not signed in", call `milanote_login`, ask the user to sign in in the window that opens, then retry.
2. Find things with `milanote_list_boards` (tree with ids and urls), `milanote_search` (text across boards) and `milanote_get_board` (full contents with element ids). To read a board as a document, use `milanote_export_board`: it returns the board as a Markdown outline, which is the best input for summaries, reviews and rewrites.
3. Build whole boards with ONE `milanote_build_board` call: pass a Markdown outline (format below) and it creates the sub-board, columns, notes, to-do lists and links in a single round trip. Use the single-element tools (`milanote_create_note`, `milanote_create_todo_list`, `milanote_add_task`, `milanote_create_link`, `milanote_create_column`, `milanote_create_board`) for additions to existing boards, and `milanote_update_element` / `milanote_move_element` for edits.
4. Ready-made structures: `milanote_templates` lists the built-in templates (gdd, character_sheet, level_design, brainstorm, moodboard, project_plan, meeting_notes, weekly_plan) and returns any of them as an outline; `milanote_apply_template` creates the empty skeleton. The prompts of this server (same names) use the templates to write a complete board with real content — prefer writing real content over leaving template text behind.
5. Finish by giving the user the board link (`https://app.milanote.com/<boardId>`, returned as `url`).

## Outline format (milanote_build_board / milanote_export_board)

```
# Board title                 ← optional; creates a new sub-board. Plain text right under it becomes the title note.
## Column title               ← a column; put related notes together, 4–8 columns of 2–6 items read best
### Note heading              ← a note; everything up to the next ### / ## belongs to it (blank lines allowed)
Body: bullets, **bold**, *italic*, `code`, > quotes, ---, and - [ ] inline checkboxes
### To-do list title          ← a section whose lines are ALL "- [ ] task" / "- [x] done" becomes a to-do list
- [ ] first task
###                           ← a bare ### starts a note (or link) without a heading
https://example.com Caption   ← a section that is only a URL (+ optional caption) becomes a link card with a preview
<!-- comments are ignored -->
```

Sections before the first `##` go straight onto the canvas (the first plain paragraph is the title note). Keep notes short — a heading plus a few bullets or one paragraph — and split long text into several notes. A `spec` object with the same shapes (`{title, header, columns:[{title, items:[{note}|{todo, tasks:[...]}|{link, caption}]}], items:[...]}`) is accepted instead of the outline.

## Text formatting inside notes and tasks

Light Markdown: `#`/`##`/`###` headings, `- ` bullets, `1. ` items, `- [ ]` checkboxes, `> ` quotes, `---` rules, **bold**, *italic*, `code`, and bare URLs become links. Line breaks are kept. There are no tables, images or nested lists.

## Positions and ids

Element ids are 14-character strings (from list/get/search or `milanote_export_board` with `ids: true`). New elements are placed automatically — to the right of existing content on a canvas, or at the end of a column; pass `x`/`y` (grid units, 1 ≈ 9 px, a note is 34 wide) or `index` to override.

## Limits and etiquette

- Free Milanote plans allow 100 notes, tasks and links in total; `milanote_whoami` reports `usage.used` / `usage.limit`. Check the headroom before building something big (a full design document is 30–50 items) and tell the user if it will not fit.
- Never trash anything the user did not ask to remove. `milanote_trash_element` moves elements to Milanote's trash (recoverable), but still ask first.
- Keep Home tidy: create new projects as a sub-board (the outline's `#` title does this) instead of spreading notes over the Home canvas.
- Write in the user's language and keep the wording of their existing notes when you move or summarise them.
