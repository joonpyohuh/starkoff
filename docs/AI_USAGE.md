# AI usage log

Tool: Claude Code (Anthropic, model Claude Opus 5.5), used inside VS Code.
The owner gave the requirements, the design choices and reviewed each step; the AI wrote code and docs
and ran the builds/tests. Every entry says what the AI did and what the owner did.

| Date | Area | What the AI did | What the owner did |
|---|---|---|---|
| 2026-11-10 | Reading | Extracted the two PDFs to text (`pdftotext`) and summarized the MVP test cases; pointed out that TC_013/014 need ChangeAvailability | Chose the MVP scope and test list |
| 2026-11-10 | Repo skeleton | Wrote `west.yml`, folder layout, this file, `DECISIONS.md` | Chose Zephyr/qemu_x86, T2 layout, no-npm web UI |
| 2026-11-10 | Firmware step 1 | Wrote prj.conf (e1000 + SLIRP, static IP, shell), Kconfig options, build/run scripts; ran QEMU and checked a TCP connect to the host | Approved the plan |
