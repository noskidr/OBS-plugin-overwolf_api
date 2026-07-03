# Continuation & handoff guide

How to pick this project up in a new chat, on a new PC, or as a new contributor.
Pairs with the repo‑root **`CLAUDE.md`** (quick AI context) — read that first, this
is the deep version.

---

## 1. What exists and where it stands

GamePulse is **complete and verified for v1 (Valorant)**:

- **OBS plugin** (`src/`, C++/Qt) — builds clean with MSVC, loads on OBS 32.1.2,
  runs the full pipeline (WS server → rules → chapters/clips/markers/overlay/
  dock/exports). Verified end‑to‑end with the simulator: a kill stream derives
  Double→Triple→Quadra→Penta + ACE and writes correct session exports.
- **Companion** (`companion/`, ow‑electron) — GEP wiring for Valorant (21640),
  normalizer, WS forwarder, tray UI, and a match simulator + headless harness.
- **CI** — Windows‑only (the product is Windows‑only). Green: build + package.
- **Reviewed** — an adversarial multi‑lens review ran; 27 distinct issues fixed,
  16 noted. See `docs/REVIEW-FINDINGS.md`.

Not done (needs external accounts / hardware — out of scope for a code session):

- A **live Valorant capture** (needs the game + Overwolf GEP whitelisting).
- A **real Twitch affiliate** test of markers/`!clip` (needs a live channel).
- **Shipping the companion publicly** (Overwolf app proposal + code‑signing cert).

The three "verify during implementation" items from the pre‑build critic
(`docs/research/08-gap-analysis-critic.md` §4) are the natural next steps.

## 2. Reproduce the environment on a fresh Windows PC

The plugin builds with the standard obs‑plugintemplate toolchain; the deps are
auto‑fetched. You need:

1. **Git**, **CMake ≥ 3.28** (the VS‑bundled one is 3.30.5 — use it), and
   **Visual Studio 2022 Build Tools** with:
   - Workload: *Desktop development with C++* (`Microsoft.VisualStudio.Workload.VCTools`)
   - **Windows 11 SDK 10.0.22621** (`Microsoft.VisualStudio.Component.Windows11SDK.22621`)

   Install headlessly:
   ```powershell
   winget install --id Microsoft.VisualStudio.2022.BuildTools --override `
     "--quiet --add Microsoft.VisualStudio.Workload.VCTools;includeRecommended `
      --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
   ```
   (If VS is already installed but missing the SDK, add it with
   `winget install --id Microsoft.WindowsSDK.10.0.22621`.)

2. **Clone + build**:
   ```powershell
   git clone https://github.com/noskidr/OBS-plugin-overwolf_api.git
   cd OBS-plugin-overwolf_api
   $cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
   & $cmake --preset windows-x64          # first run fetches OBS+Qt+curl to .deps\
   & $cmake --build --preset windows-x64
   ```
   Artifact: `build_x64\rundir\RelWithDebInfo\obs-gamepulse.dll`.

3. **A dev OBS to test against** — download portable OBS (any 30.2+; 32.1.2 used
   here) and unzip; create an empty `portable_mode.txt` in its root. Install the
   plugin into it:
   ```
   <obs>\obs-plugins\64bit\obs-gamepulse.dll
   <obs>\data\obs-plugins\obs-gamepulse\locale\en-US.ini
   ```

4. **Node 18+** for the companion / harness (`cd companion; npm install`).

## 3. The standard verification loop (no Valorant needed)

This is how every change was validated — do this after any edit:

```powershell
# 1. build (above)
# 2. install the fresh DLL into your dev OBS (copy as in step 3)
# 3. launch OBS
Start-Process '<obs>\bin\64bit\obs64.exe' -ArgumentList '--portable','--minimize-to-tray','--disable-updater'
# 4. feed a mock match through the real pipeline
cd companion; node test/harness.js --port 4477 --rounds 1
# 5. confirm:
#    - OBS log ("Help > Log Files") shows "companion connected" + no errors
#    - <obs>\config\obs-studio\plugin_config\obs-gamepulse\sessions\<ts>\session.json
#      contains kills WITH detail (e.g. "Vandal -> PhoenixDown (HS)") and derived
#      multikill_* / ace events
#    - to see chapters/clips fire, start Recording / the Replay Buffer first
```

For a live GEP session instead of the simulator: `cd companion; npm start`
(under ow‑electron), launch Valorant, set the dock's port to match.

## 4. Before pushing

CI is Windows‑only and does **not** run the formatters, so run them locally
(they're required for a tidy diff and match the `.clang-format`/`.gersemirc`):

```powershell
pip install --user "clang-format==19.1.7" gersemi
$cf = "$env:APPDATA\Python\Python310\Scripts\clang-format.exe"
Get-ChildItem src -Include *.cpp,*.h,*.hpp -Recurse | ForEach-Object { & $cf -i -style=file $_.FullName }
python -m gersemi -i CMakeLists.txt
```

Then commit with the trailers used throughout (`Co-Authored-By:` +
`Claude-Session:`), and push. Watch the run:
```powershell
Invoke-RestMethod "https://api.github.com/repos/noskidr/OBS-plugin-overwolf_api/actions/runs?per_page=1"
```

## 5. Where the knowledge lives

- **`docs/research/`** — 8 primary‑source research reports that informed every
  design decision (ow‑electron GEP API, GEP event schemas, OBS frontend API,
  plugin engineering patterns, market analysis, Twitch/YouTube constraints,
  the mined Valorant taxonomy, and the gap‑analysis critic). Start with its
  `README.md`. These are the receipts — if you're unsure why something is the
  way it is, it's cited here.
- **`PROTOCOL.md`** — the WebSocket message contract (build any producer).
- **`docs/REVIEW-FINDINGS.md`** — what the code review found and fixed.
- **`.claude/skills/obs-plugins/`** — the vendored OBS‑plugin authoring skill
  (scaffold/build/install helpers + API reference) used to build this.
- **The insights.gg teardown** that started it all lives in the parent workspace
  (`../notes/TECHNICAL_TEARDOWN.md`, `../notes/REVERSE_ENGINEERING_FINDINGS.md`) —
  not in this repo, but it's the origin of the Valorant event taxonomy.

## 6. Likely next tasks (roadmap)

1. **Live GEP validation** — play one Valorant match under `npm start`, log every
   `new-game-event`/`new-info-update`, and confirm the normalizer's name
   canonicalization holds against real data. v0.2.1 fixed a critical mismatch
   (GEP `me.player_name` carries the Riot tagline `Doom#5339`, kill_feed names
   are bare, scoreboard names are spaced `MrTest #1111`) via `baseName()`; the
   offline test `companion/test/normalizer.test.js` (`npm test`) locks the
   reconciliation/clutch/priming logic, but only a live match confirms the raw
   field formats. Also confirm round_phase values + event latency.
2. **Twitch live test** — real affiliate account: device‑flow login, burst a few
   markers, confirm `position_seconds` accuracy and the VOD‑disabled 404 path.
3. **Chapters matrix** — verify `obs_frontend_recording_add_chapter` across MKV /
   MP4 / Hybrid MP4 on 30.2 / 31 / 32 (see critic §4.1).
4. **Multi‑game** — generalize the companion normalizer beyond Valorant using the
   per‑game taxonomy already mined in `docs/research/07-valorant-event-taxonomy.md`
   (it covers ~30 games); revisit the two "multi‑game" review items.
5. **Packaging** — tag `x.y.z` to have CI cut a draft release with the Windows
   zip; write the Overwolf app proposal for the companion.
