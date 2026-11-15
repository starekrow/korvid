-- kimb: the .kimb document and what its names mean, without Krome (docs/PRDs/Krome-Slice-2-PRD.md).
-- Reading a .kimb (KBOR) with its lines, the scope a file's names resolve in (the file,
-- then its uses), the kernel's vocabulary, and the check. Core only: Krome links it to
-- load screens, kim and kimc to check them, so a mistake is found by the same rules in
-- all three places. The text form's parser is kim's.
module "kimb" {
  deps = { "core" },
}
