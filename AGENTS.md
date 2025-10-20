# Repository Guidelines

## Project Structure & Module Organization
Source lives in `fsevent/`: `main.c` wires FSEvents, LevelDB, and HTTP; `RepoMap.c` and `pb.c` handle repository bookkeeping and protocol buffers; generated headers (`vbproto_pb.h`, `pb.h`) and utilities (`uthash.h`) sit alongside. The Xcode configuration is under `fsevent.xcodeproj/`, and the LevelDB sample tool resides in `test/main.c`. A sample database snapshot (`file_index.db`) is kept at the repo root for local experimentation.

## Build, Test, and Development Commands
- `xcodebuild -scheme fsevent -configuration Debug build`: builds the macOS target using the shared Xcode scheme.
- `clang fsevent/main.c fsevent/RepoMap.c fsevent/pb.c fsevent/vbproto_pb.c -I fsevent -framework CoreServices -framework CoreFoundation -lsqlite3 -lleveldb -lpthread -o fsevent`: command-line build; adjust include/library paths if LevelDB is not in the default search path.
- `./fsevent /path/to/workspace`: runs the event listener; the path overrides the compiled `WORKSPACE` macro.

## Coding Style & Naming Conventions
Follow C99 with 4-space indentation and brace placement as in `fsevent/main.c`. Use `snake_case` for functions and variables, `CamelCase` only for typedefs mirroring system structs. Prefer static helpers for file-local utilities. Keep logging human-readable; align with existing `printf` patterns. Run `clang-format --style=file` if a `.clang-format` appears in a feature branch; otherwise format manually to match existing files.

## Testing Guidelines
There is no automated suite yet, so add focused harnesses under `test/` mirroring `test/main.c`. Compile helpers with `clang test/main.c -I fsevent -lleveldb -o bin/db_cleanup` (ensure `bin/` exists) and execute against a disposable LevelDB path. When adding features, document manual verification steps in the PR and consider instrumenting assertions around database mutations.

## Commit & Pull Request Guidelines
History mixes Conventional Commit prefixes (`fix: …`) and scoped messages (`init(fsevent): …`). Use the imperative mood, keep summaries under 72 characters, and include a short Chinese translation if it aids reviewers. For pull requests, provide: 1) a concise summary of behavior changes, 2) linked issues or tasks, 3) reproduction or validation notes (commands, logs), and 4) screenshots for UI-affecting tooling (if any). Request review once the branch is rebased onto the latest mainline and CI (if configured) passes.

## Environment & Dependencies
Requires macOS with Xcode command-line tools, LevelDB, SQLite3, and CoreServices/CoreFoundation frameworks. Before first build run `xcode-select --install` if toolchains are missing, and ensure `leveldb/c.h` is discoverable via `CPATH`/`LIBRARY_PATH`. Keep local databases and HTTP ports configurable via flags or macros; avoid committing generated DB files except for sanctioned fixtures.
