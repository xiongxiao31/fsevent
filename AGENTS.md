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
# Repository Guidelines

## Database Schema & Key Semantics
All persistent state is maintained in LevelDB. Keys follow a prefixed composite convention:
<num>:<repoid>:<time>:<eventid>

- num=1 → File event metadata.
  Value is a nanopb-encoded FileEventMeta:
    typedef struct _FileEventMeta {
        char *path;   /* Absolute file path */
        int64_t flag; /* Incremental flag used by daemon processes */
    } FileEventMeta;
  Represents a single file’s event snapshot at <time> within repository <repoid>.

- num=2 → Last recorded timestamp before previous shutdown.
  Value is a string convertible to int64_t.
  Used to guard against user-initiated system time changes that could corrupt temporal ordering.

- num=3:<repoid> → Registered repositories before last shutdown.
  Value stores a UTF-8 path string (repository root).
  Each successful registration appends an entry under this namespace.

## Protocol Buffer Interfaces
All network-facing payloads are nanopb-encoded and decoded via pb.h and generated descriptors.

- Client request (RegisterDirectoryRequest):
    typedef struct _RegisterDirectoryRequest {
        char *path;
    } RegisterDirectoryRequest;
  Sent when registering a new directory to be monitored.

- Server response (FileEventBatch):
    typedef struct _FileEventBatch {
        pb_size_t files_count;          /* number of serialized file entries */
        pb_bytes_array_t **files;       /* array of encoded FileEventMeta */
        int64_t lastUpdatedTime;        /* latest event timestamp in this batch */
        uint64_t eventId;               /* unique identifier for batch */
    } FileEventBatch;
  Used to deliver accumulated file changes with an event watermark for resumption logic.

## Encoding / Decoding Conventions
- Use nanopb for both request/response and internal LevelDB values.
- Always invoke pb_encode() / pb_decode() against the respective generated *_fields descriptors.
- Store binary value directly; do not Base64-encode for LevelDB.
- Ensure path is absolute and UTF-8 normalized before encoding.

## Repository Lifecycle & Safety
- On registration, insert a key 3:<repoid> with value=path.
- On shutdown, persist the latest time under key 2. 
- Please be sure to avoid memory leakage.
