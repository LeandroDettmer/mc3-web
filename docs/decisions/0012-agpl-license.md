# 0012: Our code moves from MIT to AGPL-3.0 with an attribution term

Date: 2026-11-13. Status: accepted (owner).

## Context

The repository was MIT. Under MIT, anyone who copies the code must keep the copyright line in the code, but nothing
more. A site that hosts a copy of the browser port does not have to name this project or publish its changes.

## Decision

- From this commit, Leandro Dettmer's code (the browser port, its page, server and tooling) is under AGPL-3.0
  (`LICENSE`).
- It carries additional terms under section 7, written out in NOTICE.md:
  - 7(b): any conveyed or network-hosted copy must show "Based on mc3-web by Leandro Dettmer" with a link to the
    repository where its users can see it.
  - 7(c): a modified version must be marked as modified.
- Section 13 already makes network hosts of a modified version offer its source.
- Third-party code keeps its licence:
  - mcdub3-nx (Laure Ladastra) stays MIT, kept in `LICENSES/MIT.txt` with her copyright line.
  - xboxrecomp stays MIT, with LGPL-2.1 parts from xemu.
  - Code reused from nfsu2-web (th0mk) stays MIT.
  - All of these are compatible with AGPL.
- Copies of earlier commits that someone already received stay MIT; a licence change cannot take that back.

## Consequences

- Our start page shows the credit and the source link (Credits tab). A fork must keep them.
- Contributions from others are AGPL-3.0 with the same terms.
