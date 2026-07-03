# CI: install PyICU from apt (python3-icu), never via pip

**Symptom:** the Pages CI job (`pages.yml` → `scripts/ci/ci.sh install-screenshot-deps`)
wedged for >20 min on the dependency-install step and timed out, after the offline
i18n pipeline started requiring `uharfbuzz`/`icu` (the complex-script work — runs.bin
generation in `build_fontpacks.py`/`hb_shaper.py`).

**Root cause:** **PyICU ships no binary wheels on PyPI.** `pip install PyICU==2.16.2`
downloads the sdist and **compiles the C++ extension from source** against the system
ICU. That build:
- needs `pkg-config` + `libicu-dev` present (or it fails with
  `KeyError: 'ICU_VERSION'` / `FileNotFoundError: 'icu-config'`/`'pkg-config'`), and
- is slow — minutes on a loaded GitHub runner — which read as a hang.

`uharfbuzz`, `fontTools`, `Pillow`, `numpy` all ship wheels, so only PyICU is the problem.

**Fix (in `scripts/ci/ci.sh::install-screenshot-deps`):** get PyICU from **apt** as the
distro-prebuilt `python3-icu` (no compile, ~12 s), and pip-install only the wheel deps
(the requirements file minus the PyICU line):

```sh
apt-get install -y ... python3-icu
grep -viE '^pyicu' tools/i18n/requirements.txt | pip3 install -r /dev/stdin
```

This works because GitHub's `ubuntu-24.04` runner uses `/usr/bin/python3`, so an
apt-installed `python3-icu` is importable by the same interpreter that runs
`build_fontpacks.py` (and pip installs into the same env — verified: `import icu` +
`import uharfbuzz` coexist). The binding tracks the **system** libicu, which is exactly
what each pack manifest's `icu_version` field already records — so using the distro
build doesn't change the reproducibility story (an ICU bump is still a reviewed
re-segmentation event). `tools/i18n/requirements.txt` keeps the pinned PyICU for local
dev where a contributor may build it deliberately.

**Why ICU is mandatory (can't just drop it):** `hb_shaper.py::line_break_indices()` and
`icu_version()` both `import icu` unconditionally for every non-RTL locale — the ICU
dictionary BreakIterator is what finds real word boundaries in no-space scripts (Thai).

See also: `pages.yml` (deploy is gated to `main` via the `github-pages` environment
branch policy, so PRs run build + screenshot-diff but never deploy).
