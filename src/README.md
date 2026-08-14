# src/

All C++ source code for the Funes binary.

```
src/
  core/           The agent harness: LLM loop, tools, memory, safety mechanisms
    tools/        Tool implementations (one file per tool or tool pack)
      generated/  HTTP-template tools scaffolded by create_tool
  server/         HTTP server, REST/SSE API, static file serving
```

The build produces one binary (`funes`) from a static library (`funes_core`,
containing everything in `core/`) linked with the server entry point and any
generated tools. See [CMakeLists.txt](../CMakeLists.txt).

- [core/README.md](core/README.md) -- the harness engine
- [core/tools/README.md](core/tools/README.md) -- every built-in tool
- [server/README.md](server/README.md) -- HTTP layer and startup
