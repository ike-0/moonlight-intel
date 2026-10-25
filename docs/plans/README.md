# Plans

Planning happens in claude.ai chats (Project "Moonlight streaming"); execution happens in
Claude Code sessions. A plan file is the hand-off between the two.

## Lifecycle

`Draft` → `Ready` → `In progress` → `In review` (PR open) → `Done` | `Abandoned`

1. Planning chat creates branch `claude/NNNN-short-slug` from `intel`, commits
   `docs/plans/NNNN-short-slug.md` (from `_template.md`, status `Ready`) and pushes it.
2. A Claude Code session is started **on that branch** with:
   *"Execute docs/plans/NNNN-short-slug.md"*.
3. The session commits code on the same branch, updates status, acceptance criteria and
   execution notes, and opens one PR into `intel` carrying plan + code.
4. The user runs the hardware test, then merges. Merged = `Done`.

Find open work with `grep -H '^| Status' docs/plans/*.md` (on `intel`, or across branches with
`git grep '^| Status' $(git for-each-ref --format='%(refname)' refs/remotes/origin/claude/) -- docs/plans`).

## Conventions

- Number sequentially, four digits; never reuse a number.
- One plan, one PR. Split anything that touches unrelated subsystems.
- Plans state measurable acceptance criteria (latency, dropped frames, decoder chosen, log line
  present) — not "works better".
- No LAN details, keys or raw logs in plans; redact as `<HOST_IP>` etc.
- No index table here: parallel plan branches would conflict on it. The plan files are the index.
