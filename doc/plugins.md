# Lua plugins

Kobalt loads plugins written in Lua 5.4. A plugin answers hooks of the reader. The only hook that changes what the reader does is `before_lookup`, which can replace a dictionary lookup with a card. Kobalt ships no plugins; installing one means copying its folder, and removing the folder removes the plugin. There is no plugin manager, settings page or persistent plugin state. [kobalt-plugins](https://github.com/daveallie/kobalt-plugins) holds an example, the X-Ray plugin, which shows who a character is from a sidecar file.

## Folders

Each plugin is a folder of its own that holds `plugin.json` and `main.lua`.

| Platform | Plugin folder |
| --- | --- |
| Kobo | `.adds/kobalt/plugins/<id>/` on user storage |
| Android | `plugins/<id>/` in the app's external files folder |
| Desktop | `plugins/<id>/` in the profile folder |

Kobalt reads the folders when the reader opens a book, on the plugin thread. A package install or upgrade never touches the Kobo folder.

## `plugin.json`

```json
{ "id": "xray", "name": "X-Ray", "version": "0.1", "api": 1 }
```

- `id` is required and uses only the characters `A-Z`, `a-z`, `0-9`, `.`, `_` and `-`. It cannot be `.` or `..`. Plugins run in order of `id`, and when several answer a lookup the first one wins. When two folders have the same `id`, Kobalt loads the first folder by name and skips and logs the other.
- `api` is required and must be `1`. Kobalt skips and logs a plugin with any other value, an invalid manifest or no `main.lua`.
- `name` defaults to the `id`. `version` is informational.
- `plugin.json` and `main.lua` may each be at most 1 MiB; Kobalt skips and logs a larger one.

## Hooks

`main.lua` runs once when the book opens. It defines global functions for the hooks it handles and can leave any of them out.

```lua
function book_opened(book) end   -- book.title, book.language
function book_closed() end
function before_lookup(ctx)      -- return nil to let the dictionary open
  return { title = "...", html = "...", footer = "..." }
end
```

`ctx` holds:

| Field | Content |
| --- | --- |
| `word` | The selected text, as the dictionary would receive it |
| `book` | The table that `book_opened` received |
| `position.percent` | 0 to 100, by word count, at the end of the visible page |
| `position.page`, `position.pages` | The page and the page count |
| `position.chapter` | The current chapter title |

`position.percent` counts the words before the first chapter, such as front matter, the words of the earlier chapters and the current chapter in proportion to its pages, up to the end of the visible page. Without word counts it falls back to page progress.

After the reader follows a link, such as an endnote, the target does not count as reached. Until the reader returns or chooses to stay, `position.page` is at most the page the link left, and `position.percent` is at most that page's share of the page count, whichever is smaller than the computed value.

A card needs a non-empty string `title` and `html`. `footer` is optional. `title` and `footer` may have at most 200 characters (UTF-16 code units) and `html` at most 64 KiB, because the sheet lays the card out on the main thread; a larger card is a failure. Kobalt shows the card in the dictionary sheet with the title in the headword style and `footer` in the line style of Dictionary sources. `title` and `footer` are plain text. `html` goes through the sheet's Qt rich-text rendering, the same as a dictionary definition: Qt's HTML subset, with no scripts and no external resources. Kobalt does not sanitise it, so a plugin must escape any text it inserts into `html`. The sheet has a book button, which closes the card and opens the normal dictionary lookup for the same word, and a close button. Returning `nil` or no value lets the dictionary open. Any other result counts as a failure.

## The `kobalt` table

| Function | Behavior |
| --- | --- |
| `kobalt.json.decode(text)` | Parses a JSON object or array into Lua tables. It returns `nil, message` on a parse error, on other top-level values, on nesting that is too deep, on text longer than an eighth of the memory cap (4 MiB) and when the decoded tables pass the memory cap. JSON `null` decodes to `nil`, so an object key with a `null` value is missing and an array with a `null` has a hole. |
| `kobalt.read_sidecar(suffix)` | Reads `<book file name without its last extension>.<suffix>` beside the book, for example `Wolf Hall.xray.json` for `Wolf Hall.epub` and `Wolf Hall.kepub.xray.json` for `Wolf Hall.kepub.epub`. It returns `nil` when the file is missing, when the suffix holds `/`, `\` or `..`, for books that are not files and always on Android, where books are imported and nothing sits beside them. |
| `kobalt.read_file(relative_path)` | Reads a file inside the plugin's own folder. It returns `nil` for absolute paths, paths that leave the folder and missing files. |
| `kobalt.log(message)` | Writes the message, cut to 1000 characters, to the diagnostic log with the plugin id. Each hook call logs at most 20 lines; the next call logs one line that says further lines were suppressed, and drops the rest. |

A file larger than the memory the plugin has left returns `nil` and a message, without being read. Plugins cannot write files, use the network or read other folders.

## Sandbox

Each plugin has its own Lua state with `base`, `string`, `table`, `math` and `utf8`. These are absent: `io`, `os`, `package`, `debug`, `require`, `dofile`, `loadfile` and `print`. `load` accepts text chunks only. `setmetatable` refuses a metatable with `__gc`, because finalizers run outside the instruction budget. Weak-keyed tables (`__mode` with `k`) keep their keys as strong references, because collecting weak keys can take the garbage collector time that grows with the square of the tables; weak values are still collected. Use `kobalt.log` instead of `print`.

## Budgets and failures

- Each hook call may run 5,000,000 Lua instructions, and a call that exceeds them stops with an error.
- A plugin may hold 32 MiB of memory; an allocation past it fails.
- The instruction budget counts only Lua instructions; library functions written in C run outside it. Kobalt therefore limits each C call whose work does not grow with memory:
  - A pattern match, such as `string.find` or `string.gsub`, stops after 1,000,000 steps per call.
  - A plain `string.find` compares at most 64 MiB of text per call.
  - `string.rep` of empty strings returns an empty string at once.
  - `table.insert`, `table.remove`, `table.concat`, `table.move` and `table.sort` refuse more than 4,194,304 elements per call. `table.sort` makes at most 64 comparisons per element, but always allows 4,194,304 and never more than 8,388,608, which sorts about 300,000 to 400,000 elements.
  - A call past one of these limits raises an error.
- A lookup waits 300 ms for a card, counted from when the reader posts it. When the time runs out, or every plugin returns `nil`, the dictionary opens. A card that arrives later is dropped.
- A plugin that raises an error, exceeds a budget or returns an invalid card has failed that call and is skipped for it. After three failures in one book, Kobalt disables it until the book is reopened. Failures go to the diagnostic log.

All plugins run on one plugin thread that owns every Lua state, so a slow plugin never delays a page turn or a tap. A lookup that arrives before `book_opened` has finished waits behind it, and that wait counts toward its 300 ms. Once the plugins have opened and none of them defines `before_lookup`, or every one that does is disabled, the dictionary opens at once without asking the plugin thread. Page changes, hiding, suspension and a new selection cancel a pending plugin lookup.

Closing a book stops a running hook: from the next instruction check, every instruction raises an error, so `pcall` cannot resume it and each `__close` handler that runs during the unwinding fails at once. A stopped hook cannot make an allocation larger than 64 KiB, so a single instruction cannot copy up to the memory cap; smaller allocations, which raising the error and unwinding need, still succeed. Closing then runs `book_closed` and waits for the plugin thread, including any C call that was already running. The limits above keep such a call to a fraction of a second on a desktop, and possibly a few seconds on a slow e-reader. They bound the wait but do not guarantee a time.

## Testing a plugin

Copy the folder to the desktop profile's `plugins/` folder, open a book and hold a word. Lua tests that need no Kobalt build can run under a stock Lua 5.4 interpreter with a stub `kobalt` table, as `kobalt-plugins` does. [Native validation](validation/dictionary.md) lists the checks of the host itself.
