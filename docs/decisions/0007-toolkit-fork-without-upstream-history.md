# 7. The toolkit is a fork without upstream history

Date: 2026-10-25

## Context

ADR 0002 imported upstream xboxrecomp `49ee746` with its history and
merged the nfsu2-sw layer and four upstream fixes with their authors
kept. On GitHub that listed upstream's contributors as contributors to
this project, which they are not: their work is in the vendored toolkit,
not in this project's commits.

## Decision

- The repository's history holds this project's commits only. The
  vendoring commit ("Vendor xboxrecomp at upstream 49ee746") adds the
  toolkit as a snapshot, with no upstream parent. History was rewritten
  on 2026-10-25 (every commit since 2026-10-11 has a new hash, signed
  again).
- Code taken from elsewhere is committed in our name, with the original
  author and source named in the message ("Imported from upstream; the
  change was written by ..."). That applies to the ten earlier imports
  (nfsu2-sw's six layers, four upstream lifter fixes) too.
- Licences stay with the code: `xboxrecomp/LICENSE` (MIT) and
  `xboxrecomp/LICENSES/` are kept, as are copyright notices in files.
- Updating: no merges from xboxrecomp-upstream or nfsu2-sw (there is no
  common ancestor now). Take a wanted change as a patch:
  `git format-patch` in a clone of upstream, then
  `git am --directory=xboxrecomp` here, then reword as above.

## Consequences

- Each upstream change is picked by hand; a large catch-up costs more
  than a merge would have. ADR 0002 already chose deliberate, rare
  updates, so little changes in practice.
- Clones from before the rewrite (the self-hosted runner's included)
  must be reset to the new `main`.
