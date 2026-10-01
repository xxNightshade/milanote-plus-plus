// McpServer.cpp - stdio MCP server
#include "McpServer.h"
#include "ServerLock.h"
#include "Updater.h"
#include "Util.h"
#include "Resource.h"
#include <io.h>
#include <fcntl.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <functional>

using nlohmann::json;

namespace mn {

namespace {

constexpr const char* kServerName = "milanote-plus-plus";
constexpr int kJsonRpcParseError = -32700;
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcMethodNotFound = -32601;
constexpr int kJsonRpcInvalidParams = -32602;
constexpr int kJsonRpcInternalError = -32603;
constexpr int kMcpResourceNotFound = -32002;

constexpr const char* kGuideUri = "milanote://guide";
constexpr const char* kTemplatesUri = "milanote://templates";
constexpr const char* kBoardsUri = "milanote://boards";
constexpr const char* kBoardUriPrefix = "milanote://board/";

// Used only if skill.md is missing from the build.
const char* kFallbackInstructions =
    "Milanote++ connects Claude to the user's Milanote workspace (visual boards with notes, columns, "
    "to-do lists, links and sub-boards). Start with milanote_whoami, find boards with milanote_list_boards, "
    "read them with milanote_get_board or milanote_export_board, and build whole boards in one call with "
    "milanote_build_board (Markdown outline: '# Board', '## Column', '### Note', '- [ ] task', bare URLs for links). "
    "If a tool answers that the user is not signed in, call milanote_login, ask the user to sign in in the "
    "window that opens, and retry.";

json Tool(const char* name, const char* description, const char* schemaJson) {
    json schema = json::parse(schemaJson);
    return json{ {"name", name}, {"description", description}, {"inputSchema", schema} };
}

const char* kParentDescription = "Id of the board or column to create the element in. Defaults to the user's Home board.";

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// prompts.json (RCDATA) parsed once: { prompts: [...], templates: {...}, snippets: {...} }
const json& Catalogue() {
    static const json catalogue = []() {
        json j = json::parse(LoadTextResource(IDR_PROMPTS_JSON), nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            LogError("prompts.json resource is missing or invalid; prompts and templates are unavailable");
            j = json::object();
        }
        if (!j.contains("prompts") || !j["prompts"].is_array()) j["prompts"] = json::array();
        if (!j.contains("templates") || !j["templates"].is_object()) j["templates"] = json::object();
        if (!j.contains("snippets") || !j["snippets"].is_object()) j["snippets"] = json::object();
        return j;
    }();
    return catalogue;
}

// Replaces every {{key}} for which resolve(key, out) returns true; other placeholders are kept verbatim.
std::string ReplacePlaceholders(const std::string& text, const std::function<bool(const std::string&, std::string&)>& resolve) {
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    for (;;) {
        size_t open = text.find("{{", pos);
        if (open == std::string::npos) { out.append(text, pos, std::string::npos); break; }
        size_t close = text.find("}}", open + 2);
        if (close == std::string::npos) { out.append(text, pos, std::string::npos); break; }
        out.append(text, pos, open - pos);
        std::string key = Trim(text.substr(open + 2, close - open - 2));
        std::string replacement;
        if (resolve(key, replacement)) out += replacement;
        else out.append(text, open, close + 2 - open);
        pos = close + 2;
    }
    return out;
}

// {{template:name}} -> the template outline, {{snippet:name}} -> the snippet text
std::string ExpandReferences(const std::string& text) {
    const json& cat = Catalogue();
    return ReplacePlaceholders(text, [&](const std::string& key, std::string& out) {
        if (key.rfind("template:", 0) == 0) {
            std::string name = Trim(key.substr(9));
            if (cat["templates"].contains(name)) { out = cat["templates"][name].value("outline", ""); return true; }
        } else if (key.rfind("snippet:", 0) == 0) {
            std::string name = Trim(key.substr(8));
            if (cat["snippets"].contains(name) && cat["snippets"][name].is_string()) { out = cat["snippets"][name].get<std::string>(); return true; }
        }
        return false;
    });
}

// {{name}} -> vars[name]; unknown names become "[name]" when bracketUnknown, otherwise stay as written
std::string ExpandVariables(const std::string& text, const json& vars, bool bracketUnknown) {
    return ReplacePlaceholders(text, [&](const std::string& key, std::string& out) {
        if (key.find(':') != std::string::npos) return false;
        if (vars.is_object() && vars.contains(key)) {
            const json& v = vars[key];
            out = v.is_string() ? v.get<std::string>() : v.dump();
            return true;
        }
        if (bracketUnknown) { out = "[" + key + "]"; return true; }
        return false;
    });
}

std::string TemplatesAsMarkdown() {
    const json& cat = Catalogue();
    std::string md = "# Milanote++ board templates\n\nUse `milanote_apply_template` to create the empty skeleton, or write real content in the same structure and create it with `milanote_build_board`.\n";
    for (auto it = cat["templates"].begin(); it != cat["templates"].end(); ++it) {
        const json& t = it.value();
        md += "\n## " + it.key() + " — " + t.value("title", "") + "\n\n" + t.value("description", "") + "\n\n";
        if (t.contains("variables") && t["variables"].is_array() && !t["variables"].empty()) {
            md += "Variables: ";
            bool first = true;
            for (const auto& v : t["variables"]) {
                if (!first) md += ", ";
                first = false;
                md += "`" + v.value("name", "") + "`";
                std::string d = v.value("description", "");
                if (!d.empty()) md += " (" + d + ")";
            }
            md += "\n\n";
        }
        std::string outline = t.value("outline", "");
        if (!outline.empty() && outline.back() != '\n') outline += '\n';
        md += "```\n" + outline + "```\n";
    }
    return md;
}

} // namespace

// ---------------------------------------------------------------- catalogue
const std::string& McpServer::Instructions() {
    static const std::string instructions = []() {
        std::string s = LoadTextResource(IDR_SKILL_MD);
        return s.empty() ? std::string(kFallbackInstructions) : s;
    }();
    return instructions;
}

json McpServer::PromptDefinitions() {
    json list = json::array();
    for (const auto& p : Catalogue()["prompts"]) {
        json args = json::array();
        if (p.contains("arguments") && p["arguments"].is_array()) {
            for (const auto& a : p["arguments"]) {
                args.push_back(json{ {"name", a.value("name", "")}, {"description", a.value("description", "")}, {"required", a.value("required", false)} });
            }
        }
        list.push_back(json{ {"name", p.value("name", "")}, {"title", p.value("title", "")}, {"description", p.value("description", "")}, {"arguments", args} });
    }
    return list;
}

json McpServer::TemplateSummaries() {
    json list = json::array();
    const json& templates = Catalogue()["templates"];
    for (auto it = templates.begin(); it != templates.end(); ++it) {
        const json& t = it.value();
        list.push_back(json{
            {"name", it.key()},
            {"title", t.value("title", "")},
            {"description", t.value("description", "")},
            {"variables", t.contains("variables") ? t["variables"] : json::array()},
        });
    }
    return list;
}

json McpServer::ToolDefinitions() {
    json tools = json::array();

    tools.push_back(Tool("milanote_whoami",
        "Returns the signed-in Milanote user (name, home board id, quick-notes board id, plan usage). Use it first to get the root board id and to check that Milanote is connected.",
        R"json({"type":"object","properties":{},"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_login",
        "Opens the Milanote++ sign-in window on the user's desktop so they can log in to Milanote. Call this when another tool reports that the user is not signed in; then ask the user to complete the sign-in and retry.",
        R"json({"type":"object","properties":{},"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_list_boards",
        "Lists boards as a tree (id, title, path, url, element counts) starting from the Home board or from boardId.",
        R"json({"type":"object","properties":{
            "boardId":{"type":"string","description":"Board to start from (default: Home board)"},
            "depth":{"type":"integer","minimum":1,"maximum":6,"default":3,"description":"How many levels of sub-boards to include"}
        },"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_get_board",
        "Reads one board: every note (as Markdown-ish text), column (with its children), to-do list (with tasks and done state), link, image and sub-board on the canvas, plus the 'Unsorted' tray, all with element ids. Pass raw=true to get Milanote's untouched element JSON instead. For a readable document form of a board use milanote_export_board.",
        R"json({"type":"object","properties":{
            "boardId":{"type":"string","description":"Board id (default: Home board)"},
            "raw":{"type":"boolean","default":false,"description":"Return the raw element map instead of the readable summary"}
        },"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_get_element",
        "Reads a single element (note, column, to-do list, task, link, board...) by id, including the id of the board it lives on.",
        R"json({"type":"object","properties":{
            "elementId":{"type":"string"},
            "raw":{"type":"boolean","default":false}
        },"required":["elementId"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_build_board",
        "Builds a whole board in ONE call from a Markdown outline: '# Title' creates a new sub-board (plain text under it becomes the title note), '## Column' starts a column, '### Heading' starts a note that runs until the next ### or ##, a ### section made only of '- [ ] task' lines becomes a to-do list ('- [x]' = done), a bare '###' followed by a URL (+ optional caption) becomes a link card with a preview, '<!-- -->' lines are ignored. Sections before the first ## go on the canvas. Notes accept light Markdown. Instead of 'outline' you may pass 'spec': {title, header, columns:[{title, items:[{note}|{todo, tasks:[string|{text,done}]}|{link, caption}]}], items:[...]}. Returns the board id/url and what was created. Prefer this over many single-element calls whenever you create more than a few elements.",
        R"json({"type":"object","properties":{
            "outline":{"type":"string","description":"The board as a Markdown outline (see the tool description for the format)"},
            "spec":{"type":"object","description":"Structured alternative to 'outline'"},
            "parentId":{"type":"string","description":"Board to build in (default: Home board). With a '# Title' line a new sub-board is created inside it; without one, the columns and notes are added to this board's canvas."},
            "newBoard":{"type":"boolean","default":true,"description":"Set false to ignore the '# Title' line and build directly on parentId"},
            "x":{"type":"number","description":"Optional canvas position (grid units) for the new board / first column"},
            "y":{"type":"number"}
        },"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_export_board",
        "Returns a board as a Markdown outline in the same format milanote_build_board accepts ('# Title', '## Column', '### Note', '- [ ] task', bare URLs for links; sub-boards are listed with their urls). The best way to read a board for summaries, reviews or rewrites. Pass ids=true to get the element ids as <!-- id --> comments for follow-up edits.",
        R"json({"type":"object","properties":{
            "boardId":{"type":"string","description":"Board id (default: Home board)"},
            "ids":{"type":"boolean","default":false,"description":"Include element ids as comments"}
        },"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_templates",
        "Lists the built-in board templates (gdd, character_sheet, level_design, brainstorm, moodboard, project_plan, meeting_notes, weekly_plan) with their variables, or returns one template's outline when 'name' is given. Use the outline as the structure for milanote_build_board and fill it with real content, or create the empty skeleton with milanote_apply_template.",
        R"json({"type":"object","properties":{
            "name":{"type":"string","description":"Template name to return in full (omit to list all)"}
        },"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_apply_template",
        "Creates a board from a built-in template as an empty skeleton (columns, guidance notes and empty to-do lists) with the template variables filled in, e.g. {name:'gdd', variables:{game:'Tic Tac Toe', platform:'Roblox'}}. To produce a finished document instead, take the outline from milanote_templates, write the real content in that structure and call milanote_build_board.",
        R"json({"type":"object","properties":{
            "name":{"type":"string","description":"Template name (see milanote_templates)"},
            "variables":{"type":"object","additionalProperties":{"type":"string"},"description":"Values for the template's {{variables}}"},
            "parentId":{"type":"string","description":"Board to create the new board in (default: Home board)"},
            "x":{"type":"number"},
            "y":{"type":"number"}
        },"required":["name"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_create_board",
        "Creates a sub-board inside a board and returns its id and URL.",
        R"json({"type":"object","properties":{
            "title":{"type":"string"},
            "parentId":{"type":"string","description":"Board to create the new board in (default: Home board)"},
            "x":{"type":"number","description":"Optional canvas position in grid units (about 9px each)"},
            "y":{"type":"number"}
        },"required":["title"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_create_note",
        "Creates a note (text card) on a board canvas or inside a column. Text supports light Markdown: '# heading' (levels 1-3), '- bullet', '1. item', '- [ ] task', '> quote', '---', **bold**, *italic*, `code`, bare URLs; line breaks are kept.",
        R"json({"type":"object","properties":{
            "text":{"type":"string","description":"Note content (light Markdown)"},
            "parentId":{"type":"string","description":"Board or column id (default: Home board)"},
            "color":{"type":"string","description":"Optional Milanote colour name for the note"},
            "x":{"type":"number","description":"Optional canvas position in grid units (boards only)"},
            "y":{"type":"number"},
            "index":{"type":"integer","description":"Optional position inside a column (0 = top)"}
        },"required":["text"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_create_column",
        "Creates a titled column on a board canvas. Notes, links and to-do lists can then be created inside it by passing the column id as parentId.",
        R"json({"type":"object","properties":{
            "title":{"type":"string"},
            "parentId":{"type":"string","description":"Board id (default: Home board)"},
            "color":{"type":"string"},
            "x":{"type":"number"},
            "y":{"type":"number"}
        },"required":["title"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_create_todo_list",
        "Creates a to-do list (optionally titled) with the given tasks, on a board canvas or inside a column. Tasks are strings, or {text, done} objects.",
        R"json({"type":"object","properties":{
            "tasks":{"type":"array","items":{"anyOf":[{"type":"string"},{"type":"object","properties":{"text":{"type":"string"},"done":{"type":"boolean"}},"required":["text"]}]},"description":"Tasks, in order"},
            "title":{"type":"string","description":"Optional list title"},
            "parentId":{"type":"string","description":"Board or column id (default: Home board)"},
            "x":{"type":"number"},
            "y":{"type":"number"},
            "index":{"type":"integer"}
        },"required":["tasks"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_add_task",
        "Adds a task to an existing to-do list.",
        R"json({"type":"object","properties":{
            "listId":{"type":"string","description":"Id of the to-do list (TASK_LIST)"},
            "text":{"type":"string"},
            "index":{"type":"integer","description":"Position in the list (default: end)"},
            "done":{"type":"boolean","default":false}
        },"required":["listId","text"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_create_link",
        "Adds a web link card (with Milanote's automatic title/preview) to a board or column.",
        R"json({"type":"object","properties":{
            "url":{"type":"string","description":"http(s) URL"},
            "parentId":{"type":"string","description":"Board or column id (default: Home board)"},
            "caption":{"type":"string","description":"Optional caption shown under the link"},
            "x":{"type":"number"},
            "y":{"type":"number"},
            "index":{"type":"integer"}
        },"required":["url"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_update_element",
        "Updates an element: 'text' for notes/tasks (or the caption of links/images), 'title' for boards/columns/to-do lists, 'done' for tasks, 'url' for links, 'color' for anything that supports colours. Only the given fields change.",
        R"json({"type":"object","properties":{
            "elementId":{"type":"string"},
            "text":{"type":"string","description":"New note/task text (light Markdown) or link caption"},
            "title":{"type":"string"},
            "done":{"type":"boolean"},
            "url":{"type":"string"},
            "color":{"type":["string","null"]}
        },"required":["elementId"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_move_element",
        "Moves an element to a board canvas, into a column, or (for tasks) into another to-do list. Position is chosen automatically unless x/y or index are given.",
        R"json({"type":"object","properties":{
            "elementId":{"type":"string"},
            "parentId":{"type":"string","description":"Destination board, column or to-do list id (default: the element's current board canvas)"},
            "x":{"type":"number"},
            "y":{"type":"number"},
            "index":{"type":"integer"}
        },"required":["elementId"],"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_trash_element",
        "Moves one or more elements (and their contents) to the board's trash, from where the user can restore them in Milanote. The Home board cannot be trashed. Only use it when the user asked for something to be removed.",
        R"json({"type":"object","properties":{
            "elementId":{"type":"string"},
            "elementIds":{"type":"array","items":{"type":"string"},"description":"Several ids at once"}
        },"additionalProperties":false})json"));

    tools.push_back(Tool("milanote_search",
        "Case-insensitive text search through notes, tasks, titles, links and captions across boards (walks sub-boards up to 'depth' levels). Returns up to 50 hits with their board path.",
        R"json({"type":"object","properties":{
            "query":{"type":"string"},
            "boardId":{"type":"string","description":"Board to search from (default: Home board)"},
            "depth":{"type":"integer","minimum":1,"maximum":6,"default":4}
        },"required":["query"],"additionalProperties":false})json"));

    return tools;
}

// ---------------------------------------------------------------- lifecycle
McpServer::McpServer(HINSTANCE instance) : instance_(instance) {}

McpServer::~McpServer() {
    stopping_ = true;
    if (reader_.joinable()) {
        // stdin is closed by the parent when the session ends; do not block shutdown on it
        reader_.detach();
    }
    if (updater_.joinable()) updater_.detach();   // a download in flight must not delay the exit either
}

int McpServer::Run() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    Log::Instance().EnableStderr(true);
    ServerLock lock;   // tells the setup window that Claude is connected (released automatically on exit)
    if (!lock.Held()) LogWarn("could not create the server-running signal (error " + std::to_string(GetLastError()) + ")");
    LogInfo(std::string("Milanote++ ") + kVersion + " MCP server starting (" + std::to_string(ToolDefinitions().size()) + " tools, "
        + std::to_string(PromptDefinitions().size()) + " prompts, " + std::to_string(TemplateSummaries().size()) + " templates)");

    WebPane::Options options;
    options.visible = false;
    options.injectBridge = true;
    options.title = L"Milanote++ (MCP host)";
    pane_ = std::make_unique<WebPane>(instance_, options);

    pane_->onAppMessage = [this](UINT msg, WPARAM, LPARAM lParam) {
        if (msg == WM_MCP_LINE) {
            std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(lParam));
            if (line) HandleLine(*line);
        } else if (msg == WM_MCP_EOF) {
            LogInfo("stdin closed, shutting down");
            PostQuitMessage(0);
        }
    };
    pane_->onClosed = []() { PostQuitMessage(0); };
    pane_->onNavigated = [](const std::wstring& url) { LogInfo("host page: " + WideToUtf8(url)); };

    WebPane::InitializeEnvironment([](HRESULT hr) {
        if (FAILED(hr)) LogError("WebView2 runtime unavailable; tool calls will fail until it is installed (https://developer.microsoft.com/microsoft-edge/webview2/)");
    });
    if (!pane_->Create()) return 1;
    pane_->Navigate(kBridgeHostUrl);

    reader_ = std::thread([this]() { ReaderThread(); });

    // Self-update in the background so people who never open the setup window still get new versions:
    // a newer release is downloaded, verified and swapped in; it is used the next time Claude starts the server.
    // Never touches stdout (that belongs to MCP). Waits a little so the first tool calls are not slowed down.
    updater_ = std::thread([]() {
        Sleep(20000);
        CleanupAfterUpdate();
        bool applied = false;
        std::string error;
        UpdateInfo info = AutoUpdate(false, {}, &applied, &error);
        if (applied) LogInfo("update " + info.latest + " installed; active after Claude restarts the server");
        else if (!error.empty()) LogInfo("update check: " + error);
    });

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    stopping_ = true;
    pane_.reset();
    return 0;
}

void McpServer::ReaderThread() {
    std::string buffer;
    std::vector<char> chunk(65536);
    HWND target = pane_ ? pane_->Hwnd() : nullptr;
    int fd = _fileno(stdin);
    for (;;) {
        // _read returns as soon as some bytes are available on the pipe (fread would wait for a full chunk)
        int n = _read(fd, chunk.data(), static_cast<unsigned>(chunk.size()));
        if (n <= 0) break;
        buffer.append(chunk.data(), static_cast<size_t>(n));
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (stopping_) return;
            HWND hwnd = pane_ ? pane_->Hwnd() : target;
            if (!hwnd || !PostMessageW(hwnd, WM_MCP_LINE, 0, reinterpret_cast<LPARAM>(new std::string(std::move(line))))) {
                return;
            }
        }
    }
    if (!buffer.empty() && !stopping_) {
        HWND hwnd = pane_ ? pane_->Hwnd() : target;
        if (hwnd) PostMessageW(hwnd, WM_MCP_LINE, 0, reinterpret_cast<LPARAM>(new std::string(std::move(buffer))));
    }
    HWND hwnd = pane_ ? pane_->Hwnd() : target;
    if (hwnd && !stopping_) PostMessageW(hwnd, WM_MCP_EOF, 0, 0);
}

// ---------------------------------------------------------------- JSON-RPC
void McpServer::WriteMessage(const json& message) {
    std::string text = message.dump();
    text.push_back('\n');
    fwrite(text.data(), 1, text.size(), stdout);
    fflush(stdout);
}

void McpServer::Respond(const json& id, const json& result) {
    WriteMessage(json{ {"jsonrpc", "2.0"}, {"id", id}, {"result", result} });
}

void McpServer::RespondError(const json& id, int code, const std::string& message) {
    WriteMessage(json{ {"jsonrpc", "2.0"}, {"id", id}, {"error", json{ {"code", code}, {"message", message} }} });
}

void McpServer::RespondToolText(const json& id, const std::string& text, bool isError) {
    Respond(id, json{ {"content", json::array({ json{ {"type", "text"}, {"text", text} } })}, {"isError", isError} });
}

void McpServer::HandleLine(const std::string& line) {
    json message = json::parse(line, nullptr, false);
    if (message.is_discarded()) {
        RespondError(nullptr, kJsonRpcParseError, "Parse error");
        return;
    }
    if (message.is_array()) {           // batch
        for (const auto& item : message) HandleMessage(item);
        return;
    }
    HandleMessage(message);
}

void McpServer::HandleMessage(const json& message) {
    if (!message.is_object()) {
        RespondError(nullptr, kJsonRpcInvalidRequest, "Invalid request");
        return;
    }
    if (message.contains("result") || message.contains("error")) return;   // a response to something we sent (we never do)

    std::string method = message.value("method", "");
    bool isNotification = !message.contains("id") || message["id"].is_null();
    json id = message.contains("id") ? message["id"] : json();
    json params = message.contains("params") ? message["params"] : json::object();

    if (method == "initialize") {
        std::string requested = params.value("protocolVersion", "");
        if (requested == "2024-11-05" || requested == "2025-03-26" || requested == "2025-06-18") {
            protocolVersion_ = requested;
        } else {
            protocolVersion_ = "2025-06-18";
        }
        initialized_ = true;
        Respond(id, json{
            {"protocolVersion", protocolVersion_},
            {"capabilities", json{
                {"tools", json{ {"listChanged", false} }},
                {"prompts", json{ {"listChanged", false} }},
                {"resources", json{ {"subscribe", false}, {"listChanged", false} }},
            }},
            {"serverInfo", json{ {"name", kServerName}, {"title", "Milanote++"}, {"version", kVersion} }},
            {"instructions", Instructions()},
        });
        return;
    }
    if (method == "notifications/initialized" || method == "notifications/cancelled" || method == "notifications/roots/list_changed") {
        return;
    }
    if (method == "ping") {
        if (!isNotification) Respond(id, json::object());
        return;
    }
    if (method == "tools/list") {
        Respond(id, json{ {"tools", ToolDefinitions()} });
        return;
    }
    if (method == "tools/call") {
        HandleToolCall(id, params);
        return;
    }
    if (method == "prompts/list") {
        Respond(id, json{ {"prompts", PromptDefinitions()} });
        return;
    }
    if (method == "prompts/get") {
        HandlePromptGet(id, params);
        return;
    }
    if (method == "resources/list") {
        Respond(id, json{ {"resources", json::array({
            json{ {"uri", kGuideUri}, {"name", "guide"}, {"title", "Milanote++ guide"}, {"description", "How to work in the user's Milanote through Milanote++: workflow, the outline format, formatting, limits."}, {"mimeType", "text/markdown"} },
            json{ {"uri", kTemplatesUri}, {"name", "templates"}, {"title", "Board templates"}, {"description", "The built-in board templates (gdd, character_sheet, level_design, brainstorm, moodboard, project_plan, meeting_notes, weekly_plan) as outlines."}, {"mimeType", "text/markdown"} },
            json{ {"uri", kBoardsUri}, {"name", "boards"}, {"title", "Board tree"}, {"description", "The user's Milanote boards as a tree with ids and urls (live)."}, {"mimeType", "application/json"} },
        })} });
        return;
    }
    if (method == "resources/templates/list") {
        Respond(id, json{ {"resourceTemplates", json::array({
            json{ {"uriTemplate", std::string(kBoardUriPrefix) + "{boardId}"}, {"name", "board"}, {"title", "Board as Markdown"}, {"description", "One Milanote board exported as a Milanote++ outline (live)."}, {"mimeType", "text/markdown"} },
        })} });
        return;
    }
    if (method == "resources/read") {
        HandleResourceRead(id, params);
        return;
    }
    if (method == "completion/complete") {
        Respond(id, json{ {"completion", json{ {"values", json::array()}, {"hasMore", false} }} });
        return;
    }

    if (!isNotification) RespondError(id, kJsonRpcMethodNotFound, "Method not found: " + method);
}

// ---------------------------------------------------------------- prompts & resources
void McpServer::HandlePromptGet(const json& id, const json& params) {
    std::string name = params.value("name", "");
    json given = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
    const json* prompt = nullptr;
    for (const auto& p : Catalogue()["prompts"]) if (p.value("name", "") == name) { prompt = &p; break; }
    if (!prompt) {
        RespondError(id, kJsonRpcInvalidParams, "Unknown prompt: " + name);
        return;
    }
    json vars = json::object();
    if (prompt->contains("arguments") && (*prompt)["arguments"].is_array()) {
        for (const auto& a : (*prompt)["arguments"]) {
            std::string argName = a.value("name", "");
            std::string value;
            if (given.contains(argName)) {
                const json& v = given[argName];
                value = Trim(v.is_string() ? v.get<std::string>() : v.dump());
            }
            if (value.empty()) {
                if (a.value("required", false)) {
                    RespondError(id, kJsonRpcInvalidParams, "Missing required argument: " + argName);
                    return;
                }
                value = a.value("default", "");
            }
            vars[argName] = value;
        }
    }
    std::string text = ExpandVariables(ExpandReferences(prompt->value("text", "")), vars, false);
    Respond(id, json{
        {"description", prompt->value("description", "")},
        {"messages", json::array({ json{ {"role", "user"}, {"content", json{ {"type", "text"}, {"text", text} }} } })},
    });
}

void McpServer::HandleResourceRead(const json& id, const json& params) {
    std::string uri = params.value("uri", "");
    auto contents = [](const std::string& u, const char* mime, const std::string& text) {
        return json{ {"contents", json::array({ json{ {"uri", u}, {"mimeType", mime}, {"text", text} } })} };
    };
    if (uri == kGuideUri) { Respond(id, contents(uri, "text/markdown", Instructions())); return; }
    if (uri == kTemplatesUri) { Respond(id, contents(uri, "text/markdown", TemplatesAsMarkdown())); return; }
    if (uri == kBoardsUri) {
        pane_->Call("milanote_list_boards", json{ {"depth", 4} }, [this, id, uri, contents](bool ok, const json& payload) {
            if (ok) Respond(id, contents(uri, "application/json", payload.dump(2)));
            else RespondError(id, kJsonRpcInternalError, payload.value("error", std::string("Milanote could not be read")));
        });
        return;
    }
    if (uri.rfind(kBoardUriPrefix, 0) == 0) {
        std::string boardId = uri.substr(strlen(kBoardUriPrefix));
        if (boardId.empty()) { RespondError(id, kMcpResourceNotFound, "Resource not found: " + uri); return; }
        pane_->Call("milanote_export_board", json{ {"boardId", boardId} }, [this, id, uri, contents](bool ok, const json& payload) {
            if (ok) Respond(id, contents(uri, "text/markdown", payload.is_string() ? payload.get<std::string>() : payload.dump(2)));
            else RespondError(id, payload.value("code", "") == "notfound" ? kMcpResourceNotFound : kJsonRpcInternalError, payload.value("error", std::string("Milanote could not be read")));
        });
        return;
    }
    RespondError(id, kMcpResourceNotFound, "Resource not found: " + uri);
}

// ---------------------------------------------------------------- tools
void McpServer::ForwardToBridge(const json& id, const std::string& tool, const json& args) {
    LogInfo("tool call: " + tool);
    // building a whole board can take a while on a slow connection
    DWORD timeoutMs = (tool == "milanote_build_board") ? 300000 : 90000;
    pane_->Call(tool, args, [this, id, tool](bool ok, const json& payload) {
        if (ok) {
            RespondToolText(id, payload.is_string() ? payload.get<std::string>() : payload.dump(2), false);
        } else {
            std::string error = payload.value("error", std::string("unknown error"));
            LogWarn("tool " + tool + " failed: " + error);
            RespondToolText(id, "Error: " + error, true);
        }
    }, timeoutMs);
}

void McpServer::HandleToolCall(const json& id, const json& params) {
    std::string name = params.value("name", "");
    json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
    if (name.empty()) {
        RespondError(id, kJsonRpcInvalidParams, "tools/call requires a tool name");
        return;
    }

    bool known = false;
    for (const auto& t : ToolDefinitions()) if (t["name"] == name) { known = true; break; }
    if (!known) {
        RespondError(id, kJsonRpcInvalidParams, "Unknown tool: " + name);
        return;
    }

    if (name == "milanote_login") {
        std::wstring exe = ExePath().wstring();
        HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), L"--login", nullptr, SW_SHOWNORMAL);
        bool ok = reinterpret_cast<INT_PTR>(r) > 32;
        std::string text = ok
            ? "A Milanote sign-in window was opened on the user's desktop. Ask the user to sign in to Milanote there (the window can be closed afterwards), then retry the previous tool."
            : "The sign-in window could not be opened. Ask the user to run Milanote++.exe (double-click it) and sign in to Milanote, then retry.";
        RespondToolText(id, text, !ok);
        return;
    }

    if (name == "milanote_templates") {
        const json& templates = Catalogue()["templates"];
        std::string wanted = args.value("name", "");
        if (!wanted.empty()) {
            if (!templates.contains(wanted)) {
                RespondToolText(id, "Error: unknown template \"" + wanted + "\". Available: " + TemplateSummaries().dump(), true);
                return;
            }
            json t = templates[wanted];
            t["name"] = wanted;
            RespondToolText(id, t.dump(2), false);
            return;
        }
        RespondToolText(id, json{
            {"templates", TemplateSummaries()},
            {"usage", "Call milanote_templates with a name to get its outline; write real content in that structure and create it with milanote_build_board, or create the empty skeleton with milanote_apply_template."},
        }.dump(2), false);
        return;
    }

    if (name == "milanote_apply_template") {
        const json& templates = Catalogue()["templates"];
        std::string wanted = args.value("name", "");
        if (wanted.empty() || !templates.contains(wanted)) {
            RespondToolText(id, "Error: unknown template \"" + wanted + "\". Available: " + TemplateSummaries().dump(), true);
            return;
        }
        json vars = args.contains("variables") && args["variables"].is_object() ? args["variables"] : json::object();
        std::string outline = ExpandVariables(ExpandReferences(templates[wanted].value("outline", "")), vars, true);
        json build = json{ {"outline", outline} };
        for (const char* key : { "parentId", "x", "y" }) if (args.contains(key)) build[key] = args[key];
        ForwardToBridge(id, "milanote_build_board", build);
        return;
    }

    ForwardToBridge(id, name, args);
}

} // namespace mn
