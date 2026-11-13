// The credits screen shows everyone CREDITS.md names: every **name** on a "By" line of
// CREDITS.md is in credits.json, and so are the lines the project requires.
#include "korvid/core/memory.h"
#include "korvid/core/format.h"
#include "korvid/platform/io.h"
#include "korvid/core/test.h"
#include <stdlib.h>
#include <string.h>

namespace {

// A staged asset of this module, beside the test executable: <exe dir>/assets/arcanum/<name>.
char* slurp(const char* name) {
    char exe[512];
    if (!kv::platform_exe_dir(exe, sizeof exe)) return nullptr; // ends in a separator
    char path[700];
    kv::format(path, sizeof path, "%sassets/arcanum/%s", exe, name);
    kv::IoBytes file;
    if (!kv::io_read_now(path, &file)) return nullptr;
    char* text = (char*)malloc(file.size + 1); // the caller's to free
    memcpy(text, file.data, file.size + 1);    // the NUL after the bytes too
    file.free();
    return text;
}

} // namespace

void test_credits(kv::TestCtx& c) {
    c.begin("credits");
    char* md = slurp("CREDITS.md");
    char* json = slurp("credits.json");
    KV_CHECK(c, md && json);
    if (!md || !json) { free(md); free(json); return; }
    unsigned names = 0;
    for (char* line = strtok(md, "\n"); line; line = strtok(nullptr, "\n")) {
        if (strncmp(line, "By ", 3) != 0) continue;
        for (char* p = strstr(line, "**"); p; ) {
            char* end = strstr(p + 2, "**");
            if (!end) break;
            *end = 0;
            ++names;
            if (!strstr(json, p + 2)) { KV_CHECK(c, false); kv::print_err("credits.json does not name '%s'\n", p + 2); }
            p = strstr(end + 2, "**");
        }
    }
    KV_CHECK(c, names >= 2);
    const char* const required[] = {"starekrow", "Claude Code", "Coding Assistance", "Built with Korvid Engine", "CC0"};
    for (const char* r : required) KV_CHECK(c, strstr(json, r) != nullptr);
    free(md);
    free(json);
}
