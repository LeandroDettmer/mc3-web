<!-- Draft only: not posted. Review and post yourself if you agree. -->

**Title:** License for the port code, and interest in upstreaming the Switch layer?

Hi! Thanks for nfsu2-sw. It's the clearest example of an xboxrecomp port
running natively on the Switch, and the notes in CLAUDE.md / PERF_NOTES.md
are excellent.

I'm starting a similar port (Midnight Club 3: DUB Edition, Xbox) and vendor
the Switch-enabled toolkit from your `xboxrecomp/` directory, credited to you
commit by commit. Two questions:

1. The repo has no top-level licence, so the code in `src/`, `switch/` and
   `tools/` is all-rights-reserved by default. Would you consider adding one
   (MIT, like the toolkit, would make reuse simplest)?
2. Would you be open to upstreaming the Switch layer (nv2a_gl / nv2a_vk,
   xinput_nx, the Horizon memory and threading work) into
   sp00nznet/xboxrecomp? Every Switch port could then share one copy. I'm
   happy to help with that.

Also, in case it's useful: the published `xboxrecomp/` is missing
`tools/disasm/output.py`, `tools/func_id/output.py` and
`tools/recomp/output.py` (present upstream), so `python3 -m tools.disasm`
fails to import from a fresh clone.
