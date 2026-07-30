// =============================================================================
// src/core/tools/result_tools.cpp — read_result native tool
// =============================================================================
// The dereference half of the result store (see core/result_store.h). When a
// tool returns more than kInlineLimit bytes, the transcript gets a preview
// carrying a result_id; this is how the model gets the rest.
//
// Windowed rather than whole-value on purpose: handing back the full 40KB on
// the first dereference would re-import exactly what the preview evicted, and
// the model would be no better off than before. A window also lets it walk a
// long document in bounded steps, which is what it usually wants anyway.

#include "../memory.h"
#include "../result_store.h"
#include "../tools.h"
#include <algorithm>

namespace {

ToolResult read_result_handler(MemoryStore& store, const json& args, const ToolContext& ctx) {
    if (!args.contains("id") || !args["id"].is_number_integer())
        return {"Missing or non-integer 'id' argument", true};

    const int64_t id = args["id"].get<int64_t>();
    const int64_t raw_offset = args.value("offset", 0);
    const int64_t raw_limit  = args.value("limit", static_cast<int64_t>(funes::kInlineLimit));

    if (raw_offset < 0) return {"'offset' must not be negative", true};
    if (raw_limit <= 0) return {"'limit' must be positive", true};

    // Clamped so this tool's own output can never cross the storage threshold
    // and get stored in turn.
    const size_t limit = std::min<size_t>(static_cast<size_t>(raw_limit), funes::kInlineLimit);

    std::optional<std::string> text = store.get_result(ctx.session, id);
    if (!text)
        return {"No stored result with id " + std::to_string(id) +
                " in this conversation. Result ids come from the previews of "
                "large tool results and are only valid in the session that "
                "produced them.", true};

    return {funes::result_window(*text, static_cast<size_t>(raw_offset), limit)};
}

} // namespace

void register_result_tools(ToolRegistry& reg, MemoryStore& store) {
    reg.add({
        "read_result",
        "Read part of a large tool result that was stored instead of being shown in full. "
        "When a tool returns a lot of data you get a preview containing a result_id, its "
        "size in bytes, and the start and end of the content; call this with that id to "
        "read the rest, a window at a time. Returns at most " +
            std::to_string(funes::kInlineLimit) +
            " bytes per call and tells you the offset to continue from.",
        {
            {"type", "object"},
            {"properties", {
                {"id",     {{"type", "integer"},
                            {"description", "result_id from the preview of a stored tool result"}}},
                {"offset", {{"type", "integer"},
                            {"description", "Byte offset to start reading at (default 0)"}}},
                {"limit",  {{"type", "integer"},
                            {"description", "How many bytes to read, capped at " +
                                            std::to_string(funes::kInlineLimit)}}}
            }},
            {"required", json::array({"id"})}
        },
        [&store](const json& args, const ToolContext& ctx) {
            return read_result_handler(store, args, ctx);
        }
    });
}
