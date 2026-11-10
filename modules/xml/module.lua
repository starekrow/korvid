-- XML reading: yxml (MIT), vendored and pinned, behind a Korvid tree API in the shape of
-- the JSON reader's. Non-validating, UTF-8 only, no entities but XML's five, no DTD
-- applied. Present only in an executable that depends on it (arcfire's BulletML).
module "xml" {
  deps = { "core" },
  licenses = { "src/vendor/COPYING" },
  private_includes = { "src/vendor" }, -- yxml.c includes <yxml.h>
}
