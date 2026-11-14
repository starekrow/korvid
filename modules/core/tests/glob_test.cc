#include "korvid/core/glob.h"
#include "korvid/core/test.h"

void test_glob(kv::TestCtx& c) {
    using namespace kv;
    c.begin("glob");

    // Literals and the single-segment wildcards.
    KV_CHECK(c, glob_match("ship.png", "ship.png") && !glob_match("ship.png", "ship.pn") && !glob_match("ship.pn", "ship.png"));
    KV_CHECK(c, glob_match("*.png", "ship.png") && glob_match("*", "ship.png") && !glob_match("*.png", "ship.json"));
    KV_CHECK(c, glob_match("*.material.json", "rock.material.json") && !glob_match("*.material.json", "rock.json"));
    KV_CHECK(c, glob_match("s?ip.*", "ship.png") && !glob_match("s?ip.*", "sip.png"));
    KV_CHECK(c, glob_match("a*b*c", "aXbYc") && glob_match("a*b*c", "abc") && glob_match("a*b*c", "abbbc") && !glob_match("a*b*c", "abcd"));
    KV_CHECK(c, glob_match("*a*a*a", "aaaa") && !glob_match("*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaa"));
    KV_CHECK(c, glob_match("", "") && !glob_match("", "a") && !glob_match("a", "") && glob_match("*", ""));

    // A star never crosses a slash; neither does ? or a class.
    KV_CHECK(c, !glob_match("*.png", "ships/big.png") && glob_match("*/*.png", "ships/big.png") && !glob_match("*/*.png", "a/b/big.png"));
    KV_CHECK(c, !glob_match("a?b", "a/b") && !glob_match("a[/]b", "a/b") && !glob_match("a[!x]b", "a/b"));

    // Classes: sets, ranges, negation, a literal ] first, an escape.
    KV_CHECK(c, glob_match("ship[0-9].png", "ship7.png") && !glob_match("ship[0-9].png", "shipA.png"));
    KV_CHECK(c, glob_match("[abc]x", "bx") && !glob_match("[abc]x", "dx") && glob_match("[!abc]x", "dx") && !glob_match("[!abc]x", "ax"));
    KV_CHECK(c, glob_match("[]]x", "]x") && glob_match("[a\\]]x", "]x") && glob_match("[a-c-]x", "-x"));
    KV_CHECK(c, !glob_match("[abc", "a") && !glob_match("x[", "x["));
    KV_CHECK(c, glob_match("\\*.png", "*.png") && !glob_match("\\*.png", "a.png") && glob_match("a\\?", "a?"));

    // ** as a whole segment.
    KV_CHECK(c, glob_match("bosses/**", "bosses/a.png") && glob_match("bosses/**", "bosses/x/y/a.png") && !glob_match("bosses/**", "bosses"));
    KV_CHECK(c, !glob_match("bosses/**", "bossesx/a.png") && !glob_match("bosses/**", "ui/bosses/a.png"));
    KV_CHECK(c, glob_match("**/a.png", "a.png") && glob_match("**/a.png", "x/y/a.png") && !glob_match("**/a.png", "x/y/b.png"));
    KV_CHECK(c, glob_match("a/**/b", "a/b") && glob_match("a/**/b", "a/x/b") && glob_match("a/**/b", "a/x/y/b") && !glob_match("a/**/b", "a/x/c"));
    KV_CHECK(c, glob_match("**/*.png", "a.png") && glob_match("**/*.png", "k/s/a.png") && glob_match("**", "a/b") && glob_match("**", "a"));
    KV_CHECK(c, glob_match("a/**/**/b", "a/b") && glob_match("**/x/**", "a/x/b") && !glob_match("**/x/**", "a/x"));
    // Two stars inside a segment are two single stars.
    KV_CHECK(c, glob_match("a**b", "aXXb") && !glob_match("a**b", "a/b"));

    // A rule's pattern: no slash means the file's name at any depth.
    KV_CHECK(c, glob_match_path("*.png", "ship.png") && glob_match_path("*.png", "kenney/ships/big.png") && !glob_match_path("*.png", "a/b.json"));
    KV_CHECK(c, glob_match_path("ship.png", "x/ship.png") && !glob_match_path("ship.png", "x/aship.png"));
    KV_CHECK(c, glob_match_path("kenney/**", "kenney/ships/big.png") && !glob_match_path("kenney/**", "a/kenney/big.png"));
    KV_CHECK(c, glob_match_path("ui/*.json", "ui/hud.json") && !glob_match_path("ui/*.json", "x/ui/hud.json") && !glob_match_path("ui/*.json", "ui/a/hud.json"));

    KV_CHECK(c, glob_is_literal("ships/big.png") && !glob_is_literal("*.png") && !glob_is_literal("a[b]") && !glob_is_literal("a?"));
    KV_CHECK(c, !glob_match(nullptr, "a") && !glob_match("a", nullptr));
}
