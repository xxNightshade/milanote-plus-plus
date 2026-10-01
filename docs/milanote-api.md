# Milanote private API notes

Everything below was captured on 2026‑09‑30 from the Milanote web app (version 3.18.132) by
watching its network traffic and reading its (minified) client bundle. **Milanote has no public
API** — this is the app's own private protocol. It can change at any time without notice and
using it may not be covered by Milanote's terms of service; Milanote++ uses it at your own risk.

## Hosts and auth

| What | Value |
| --- | --- |
| Web app / REST | `https://app.milanote.com` (REST under `/api/…`, `apiRoot` is empty) |
| Media service | `https://app.milanote.com/media-service/api/…` (link previews, image resize/thumb, board preview) |
| Realtime | `wss://app.milanote.com/socket.io/?userId=<userId>&EIO=4&transport=websocket` |
| Collaboration (cursors, live text) | `https://app.milanote.com/collab/socket.io?channel=&userId=` (not used by Milanote++) |
| Static assets / WAF | `https://static.milanote.com/…` |
| Uploaded media | `https://media.milanote.com/…` (S3 folder `p` in production) |

Authentication is a **session cookie** (Better Auth, HttpOnly). Requests made from a page on the
`app.milanote.com` origin with `credentials: "include"` are authenticated automatically; without
the cookie the API answers `401 {"error":{"name":"UnauthorizedError",…,"details":{"authFailureReason":"missing-authentication"}}}`.

The cookie is a *browser-session* cookie: it is gone as soon as the browser process exits. What
makes logins stick is a long-lived JWT the app stores in `localStorage.token` (payload
`{_id, iat, platform, role, rootBoardId}`, no expiry). It is accepted as `Authorization: Bearer <token>`
on REST calls, and `POST /api/auth/upgrade-better-auth-session` (same header, empty JSON body) answers
`{"success":true}` and sets a fresh session cookie. Milanote++ does exactly that whenever it sees a
401 (the WebSocket needs the cookie, headers cannot be set on it).

Login itself is `POST /api/auth/login` (email/password + **reCAPTCHA v3** token) or Google/Apple
sign‑in redirects (`/api/auth/google/sign-in-redirect`, `/api/auth/apple/callback`). Because of the
captcha the only sane way to log in is Milanote's real login page, which is what Milanote++ shows
in its WebView2 window.

The site is fronted by **AWS WAF** with a JavaScript challenge. The app loads
`https://static.milanote.com/awswaf/<id>/challenge.compact.js` (id was `01e5d84276c0`), which
exposes `window.AwsWafIntegration` (`getToken()`, `hasToken()`, `forceRefreshToken()`, `fetch()`)
and stores the token in the `aws-waf-token` cookie. A challenge response has the
`x-amzn-waf-action` header. The bridge loads the same script and refreshes the token if it ever
sees such a response.

## REST endpoints (GET unless noted)

| Endpoint | Notes |
| --- | --- |
| `/api/users/me` | `{user:{_id,email,name:{displayName,…},rootBoardId,quickNotesRootId,clientId,clientTick,contentLimit,settings,…}, analytics, elements}` |
| `/api/users/me/counts` | `{counts:{BOARD:1,…}, totalUsage, contentLimit:{default,current,exceeded,…}, isUnlimited}` — free plan = 100 notes |
| `/api/users/me/app-init?timezone=<minutes>` | boot payload: `boardHierarchies`, `elements` (root boards), `cache`, `remoteUserIds` |
| `/api/users/me/subscription`, `/api/users/me/notification-preferences`, `/api/users/me/custom-templates-init`, `/api/users/me/trash?other-users=false` | misc |
| `/api/boards?excludeSelf=false&loadAncestors=false&ids=<id>[,<id>]` | **board contents**: `{elements:{id:Element}, comments, labels, errors, childrenReturned, canvasOrder, boardIds, elementCount, commentCount, errorCount, fetchedTime}`. Returns the board plus its whole subtree except the contents of child boards. `canvasOrder=true` adds z‑order lists. `tokens=` carries share tokens for boards you only have a link to. |
| `/api/boards/modified?ids=<ids>` | `{modifiedTimes:[{id,modifiedTime}]}` (poll used by the app) |
| `/api/elements?ids=<ids>&loadAncestors=true` | `{elements, comments, labels, errors, ids, boardInfo, readableAncestorIdsMap}` — with `loadAncestors` the parent chain (up to the root board) is included, which is how the bridge finds the board an element lives on |
| `/api/elements/counts`, `/api/elements/duplicate`, `/api/elements/attachments/presign`, `/api/elements/<id>/permissions`, `/api/elements/<id>/publish`, `/api/elements/<id>/editors`, `/api/elements/<id>/clones` | not used |
| `/api/users?ids=`, `/api/users/activity?ids=`, `/api/notifications?after=0`, `/api/labels`, `/api/comments`, `/api/comments/threads`, `/api/version/status?version=&appType=web`, `/api/ping`, `/api/locales`, `/api/cms/releases?platform=Web` | misc |
| `POST /api/actions {action}` | accepts `{"success":true}` for *analytics‑style* actions (e.g. `USER_NAVIGATE`); **element mutations sent here are silently ignored** — they must go over the socket |
| `POST /media-service/api/link {url, elementId, environmentFolder:"p", userId, locale}` | link preview → `{image:{regular,large,width,height,thumb,transparent}, mediaType:"IMAGE", link:{url,title,favicon}, elementType:"LINK"|"IMAGE"|"ALIAS", description, provider:{url,name,display}}`. Does **not** modify the element; the client applies the result with `ELEMENT_SET_TYPE`. |
| `/media-service/api/preview/<boardId>`, `POST …/image/resize`, `…/image/thumb?url=` | not used |

### Element shape

```json
{
  "_id": "1XbZEqlawr6y03",
  "elementType": "CARD",
  "location": { "parentId": "1XbZEplawr6y02", "section": "CANVAS", "position": { "x": 4, "y": 2, "score": 65536 } },
  "content": { "textContent": { "type": "doc", "content": [ … ] } },
  "meta": { "creator": "<userId>", "modifiedBy": "<userId>", "createdTime": 1790795254321, "modifiedTime": 1790795254321,
            "platform": "Desktop web", "locationSectionModifiedTime": 1790795254321, "versionId": "1XbZ5itODK-3", "significantModified": … },
  "acl": { … only on shared boards … }
}
```

* **Element types**: `BOARD, CARD (note), COLUMN, TASK_LIST, TASK, LINK, LINE, IMAGE, FILE, COMMENT_THREAD, ALIAS, CLONE, SKETCH, ANNOTATION, COLOR_SWATCH, DOCUMENT, TABLE` (+ internal `SKELETON`, `UNKNOWN`).
* **Sections**: `CANVAS` (free placement on a board, position `{x, y, score}` in grid units — 1 unit ≈ 9 px at 100 % zoom, a default note/column is 304 px ≈ 34 units wide; `score` is the z‑order, the app uses `max(sibling scores) + 65536`), `INBOX` (ordered list inside a column / to‑do list / the board's "Unsorted" tray, position `{index, score:index*65536}`), `ATTACHED` (comments attached to an element), `TRASH` (`parentId` = `<boardId>-trash`, position `{addedDate, restoreLocation}`), `DELETED`, `QUICK_NOTES`.
* **Content fields** seen in the client's schema: `title, showTitle, color, secondaryColor, icon, width, height, autoHeight, caption, showCaption, textContent, keywords, link{url,favicon,type}, url, image{regular,large,huge,original,thumb,width,height,transparent,colors}, media, mediaType, file{filename,size,url,mime}, isComplete, assignments, dueDate, collapsed, locked, defaultColorPalette, background, start/end/control/lineStyle (lines), drawing, reactions, labels, tableContent…`
* Defaults used on creation: BOARD `{title:null, icon:null, color:null, secondaryColor:null, defaultColorPalette:[7 hex colours]}`, CARD `{textContent:null}`, COLUMN `{title:null}`, TASK_LIST `{title:null, showTitle:false}`, TASK `{textContent:null}`, LINK `{url:null}`.

### Rich text (`textContent`, `caption`)

TipTap/ProseMirror JSON. Nodes: `doc, paragraph{key,textAlign}, heading{key,textAlign,level}, bulletList{key}, orderedList{key,start,type}, listItem{key}, taskList{key}, taskItem{key,checked}, blockquote{key}, callout{key,icon,iconCode}, codeBlock{key,language}, smallText, horizontalRule, hardBreak, text, mention{label}, inlineBoardLink{url,boardId,title,color,permissionId,status}`.
Marks: `bold, italic, underline, strike, code, link{href,target,rel,class}, highlight{color}, textStyle{color}, textMention{mentionKey,userId,originalText}, inlineComment{commentThreadId}`.
Every block carries a random 5‑character `key`. Example note:

```json
{"type":"doc","content":[{"type":"paragraph","attrs":{"key":"YUt42","textAlign":"left"},"content":[{"type":"text","text":"Hello"}]}]}
```

### IDs

`<6 chars base62(unix seconds)><6 char clientId><2 chars base62(clientTick % 3844)>` with the
alphabet `0-9a-zA-Z`. `clientId` is random per client, `clientTick` increments per created id.
Device ids look like `mdid-<6 chars time><4 random>`, session ids `msid-<6><4>`, and element
version ids are `<session id without prefix>-<n>` (`n` = 1 on create, incremented on every update
by the client that made it; updates also send the previous id as `modifiedVersionId`).

## Realtime protocol (writes)

Socket.IO v4 over a raw WebSocket (Engine.IO v4 framing):

```
<- 0{"sid":…,"pingInterval":25000,"pingTimeout":20000,"maxPayload":"1e7"}   open
-> 40                                                                     connect default namespace
<- 40{"sid":…}
<- 2   /  -> 3                                                            ping / pong
-> 42<ackId>["action", {…action…}]                                        send an action
<- 43<ackId>[{"status":200}]                                              ack
<- 42["action", {…, "remote":true, "source":"REMOTE"}]                    broadcast of other clients' actions
-> 42<ackId>["update-channels", {"joined":["<boardId>","<boardId>-LIVE"], "left":[]}]   subscribe
```

Channels: `<boardId>` (persistent element changes), `<boardId>-LIVE` (selection, cursors),
`<userId>` / `<userId>-PERSONAL` (user level). Actions are only broadcast to sockets that joined
the channels listed in the action's `channels` array.

Every action carries an envelope:

```json
{ "type": "…", "sync": true, "timestamp": 1790795254321,
  "user": { "_id": "<userId>", "clientId": "1WkDRF", "clientTick": 950 },
  "tokens": [], "deviceId": "mdid-…", "sessionId": "msid-…",
  "activity": { "track": false, "boardId": "<boardId>" }, "channels": ["<boardId>"] }
```

Captured action types (the ones Milanote++ uses):

* **ELEMENT_CREATE** — `{id, elementType, location, content, meta:{creator, modifiedBy, createdTime, modifiedTime, platform, locationSectionModifiedTime, versionId}, creationSource:"TOOLBAR"|"DOUBLE_CLICK"|…, markAsFetched}`. `markAsFetched:true` tells other clients the element is fully known; for a **board that will be filled afterwards send `false`**, otherwise open web apps show it empty until the user re‑opens it.
* **ELEMENT_UPDATE** — `{updates:[{id, changes:{…content fields…}, meta:{modifiedVersionId, versionId}}], recalculateChannels:false, activity:{track:false, boardId, elementTypes:{id:type}}, channels:[boardId, id]}`.
* **ELEMENT_MOVE_MULTI** — `{moves:[{id, location:{parentId, section, position}, from:{parentId, section, position}}], moveOperation:"DROP"|"TRASH"|"COLLISION"|…, monitoring:{operation}, activity:{track:false, sourceBoardId, isSourceShared, destinationBoardId, isDestinationShared, elementTypes}}`. Trashing = a move to `{parentId:"<boardId>-trash", section:"TRASH", position:{addedDate, restoreLocation:<old location>}}` with `moveOperation:"TRASH"`. Children (a column's notes) move with their parent.
* **ELEMENT_SET_TYPE** — `{id, elementType, changes}` (used after the link preview fetch: `changes = {image, mediaType, link, provider, caption, showCaption}`).
* Also seen: `ELEMENT_DELETE {id, location, elementType}` (permanent), `ELEMENT_DIFF_UPDATE`, `ELEMENT_MOVE_AND_UPDATE`, `ELEMENT_DUPLICATE`, `ELEMENT_UPDATE_ACL`, `ELEMENTS_SELECTED / ELEMENTS_DESELECTED / ELEMENTS_DESELECT_ALL` (LIVE channel), `USER_NAVIGATE`, `USER_UPDATE`, `USER_ACTIVITY_HEARTBEAT`, `COMMENTS_*`, `BOARD_*`, `TRASH_*`.

## Client behaviour worth knowing

* The web app caches boards locally (IndexedDB) and only re‑fetches a board when
  `/api/boards/modified` reports a newer `modifiedTime` than its cached fetch time, or when it is
  subscribed to the board's channel and receives the action live.
* A "Choose a template" dialog is shown for boards the app believes are empty.
* The free plan is limited to 100 notes (`contentLimit`); creation beyond the limit is refused by
  the app (the server may still accept it — Milanote++ does not enforce it).
