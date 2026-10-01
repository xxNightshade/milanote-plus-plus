# Tic Tac Toe — Game Design Document
v1 · Read the columns left to right: Overview → Rules → AI → UX → Art & audio → Technical design → Scope → References.

## 1 · Overview
### Tic Tac Toe
**Working title:** Tic Tac Toe
**Genre:** Abstract strategy / casual
**Players:** 1 (vs AI) or 2 (hot-seat)
**Session length:** 30–90 seconds per match
**Platforms:** PC + mobile (touch and mouse); engine-agnostic design
**Status:** v1 design

### Elevator pitch
The classic 3×3 game rebuilt to feel instant and satisfying: one tap to start, snappy marks, an AI that is fair at every difficulty, and a rematch loop that keeps a best-of series going without a menu in the way.

### Design pillars
- **Instant** — from launch to first move in under 3 seconds; no menus between rematches.
- **Readable** — the board state is obvious at a glance on any screen size.
- **Fair and honest** — the AI never cheats; its difficulty is real, not random.
- **Snappy** — every input gets immediate visual and audio feedback.

### Goals & non-goals
**Goals (v1)**
- A complete, polished game that is fun for 5 minutes.
- Reusable architecture: rules, AI and UI are separate modules with tests.
- Runs on PC and mobile from the same code.

**Non-goals (v1)**
- Online multiplayer, accounts or monetisation.
- Board sizes other than 3×3 (Ultimate Tic Tac Toe is a possible v2).

## 2 · Rules & core loop
### Rules
- 3×3 grid. X moves first in the first match; after that the loser of the previous match starts.
- Players alternate placing one mark in an empty cell.
- Three marks in a row (horizontal, vertical or diagonal) wins.
- A full board with no line is a draw.
- Illegal input (occupied cell, wrong turn) is rejected with a small shake — never a modal.

### Core loop
Title → Play → Match (5–9 turns) → Result overlay → Rematch or Change mode
- The result overlay shows the winning line, updates the series score and offers **Rematch** (Space / tap) and **Menu**.
- Getting from the result back into a new match must never take more than two inputs.

### Game modes
- **Local 2P** — hot-seat on one device.
- **vs AI** — Easy / Normal / Unbeatable.
- **Series** — first to 3 wins; draws count for nobody.
- *Stretch:* daily puzzle ("find the winning move") with a shareable result.

### Win / draw detection
- 8 winning lines: 3 rows, 3 columns, 2 diagonals.
- After each move only the lines through the last cell are checked (at most 4).
- Draw when 9 marks are placed and no line exists.
- The result is final the moment it is detected; no further input is accepted until Rematch.

## 3 · AI
### Difficulty tiers
- **Easy** — picks a random legal cell and never blocks. Lets new players win.
- **Normal** — wins if it can, blocks if it must, otherwise random. Beatable with a fork.
- **Unbeatable** — full minimax; never loses and wins whenever the player slips.

### Minimax (Unbeatable)
- Score: win = +10 − depth, loss = depth − 10, draw = 0. Depth-aware so it wins fast and loses slow.
- The full 3×3 tree is tiny (at most 9! = 362,880 leaves, far fewer once wins prune it), so no memoisation is needed.
- Ties between equally good moves are broken at random so games do not feel scripted.
- The reply is delayed by 250–400 ms so the AI feels like it is thinking.

### AI feel
- The AI never appears to cheat: it moves only on its turn and only into legal cells.
- Easy deliberately misses obvious blocks about 20 % of the time.
- A small "thinking" indicator shows during the delay so the wait reads as intentional.

## 4 · UX & UI
### Screen flow
- **Title** — Play, Mode, Settings. One tap on Play starts the last-used mode.
- **Match** — board, turn indicator, series score, Menu.
- **Result overlay** — winner or draw, winning line highlighted, Rematch / Menu.
- **Settings** — sound, theme, reduced motion, AI difficulty.

### Board & marks
- The board takes about 70 % of the shorter screen edge; cells are at least 64 px on touch screens.
- Marks draw in with a 120 ms stroke animation.
- Hover / press shows a ghost mark in the current player's colour.
- The winning line sweeps across the three cells in 200 ms.

### Feedback & juice
- Mark placed: short tick sound + scale pop.
- Illegal move: 80 ms shake + low thud.
- Win: line sweep, confetti burst (skippable), jingle.
- Draw: board dims, soft tone.
- The turn indicator glows for the active player.

### Accessibility
- Keyboard: 1–9 map to cells (numpad layout); Enter / Space confirms Rematch.
- Marks differ by shape as well as colour, so nobody has to rely on colour alone.
- The reduced-motion setting disables the sweep, pop and confetti.
- Screen-reader labels: "Cell 5, empty" / "Cell 5, X".

## 5 · Art & audio
### Visual style
- Flat, high-contrast, generous whitespace; Light and Dark themes.
- Palette: background `#F6F4EE`, grid lines `#2B2B2B`, X `#E4572E`, O `#1B9AAA`, accent `#F7B801`.
- Rounded line caps everywhere; marks are drawn strokes, not sprites.

### SFX list
- [ ] Mark placed — X (short tick)
- [ ] Mark placed — O (softer tick)
- [ ] Illegal move (low thud)
- [ ] Win jingle (about 1.5 s)
- [ ] Draw tone
- [ ] UI tap
- [ ] Optional ambient loop (off by default)

## 6 · Technical design
### Architecture (separation of concerns)
- **GameState** — plain data: board, turn, result, scores.
- **Rules** — pure functions: `legalMoves`, `applyMove`, `winner`, `isDraw`.
- **AI** — `chooseMove(state, difficulty)`; depends only on Rules.
- **Session** — owns the current match and series; the only thing that mutates state.
- **View / UI** — renders state and sends intents (cell tapped, rematch).
- **Audio** and **Persistence** — subscribe to Session events.

Data flows UI → Session → Rules / AI. Rules and AI are unit-tested without any UI.

### Data model
- `board: Cell[9]` where Cell = "" | "X" | "O", index 0–8 row-major
- `turn: "X" | "O"`
- `result: { winner: "X" | "O" | "draw" | null, line?: [i, j, k] }`
- `mode: "local2p" | "vsAI"`, `difficulty: "easy" | "normal" | "unbeatable"`
- `scores: { X, O, draws }`
- `settings: { sound, theme, reducedMotion }`

### Events
- `matchStarted { starter }`
- `markPlaced { cell, mark }`
- `illegalMove { cell, reason }`
- `turnChanged { turn }`
- `aiThinking { on }`
- `matchEnded { result }`

### Roblox notes (if built in Luau)
- Rules and AI as ModuleScripts in ReplicatedStorage, tested with TestEZ.
- Session runs on the server for 2P (one board per seat pair) and on the client for vs AI.
- UI in StarterGui driven by a small state store; RemoteEvents carry only cell indices.
- Scores persist in a DataStore keyed by UserId behind a ProfileStore-style wrapper.

## 7 · Scope & milestones
### Milestone 1 — Playable core
- [ ] Board rendering and input (mouse, touch, keyboard)
- [ ] Rules module with unit tests for all 8 lines and draws
- [ ] Local 2P match with result overlay
- [ ] Rematch flow (loser starts)

### Milestone 2 — AI
- [ ] Easy, Normal and Unbeatable tiers
- [ ] Thinking delay and indicator
- [ ] Series scoring (first to 3)

### Milestone 3 — Polish
- [ ] Animations and SFX
- [ ] Light / Dark themes
- [ ] Accessibility pass (keyboard, reduced motion, labels)
- [ ] Settings and persistence

### Risks & open questions
- **Too little game** — mitigate with Series mode, AI tiers and the daily-puzzle stretch goal.
- **Normal AI feels random** — tune the block rate in playtests.
- **Scope creep** — Ultimate Tic Tac Toe and online play are explicitly v2.
- *Open:* should X always start, or the loser of the last match? Current call: loser starts.

## 8 · References & playtest
###
https://en.wikipedia.org/wiki/Tic-tac-toe Rules, strategy and the perfect-play analysis

### Playtest checklist
- [ ] A new player understands the rules without reading anything.
- [ ] Time from launch to first move is under 3 seconds.
- [ ] Normal AI loses to a fork at least half the time.
- [ ] Unbeatable AI never loses in 50 matches.
- [ ] Rematch never takes more than two inputs.
- [ ] Every input has visible feedback within one frame.
