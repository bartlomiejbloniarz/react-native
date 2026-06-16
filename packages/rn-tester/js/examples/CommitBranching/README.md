# `enableFabricCommitBranching` — BeforeWaiting starvation repro

A minimal RNTester example showing how `enableFabricCommitBranching` can cap React-driven
update latency/throughput when the main run loop is kept busy. One screen: a flag badge, a
"Jank" switch, an "Update" button, and an "Auto-update" switch.

RNTester → **APIs** tab → **Commit Branching jank**.

---

## 1. Build & run with the flag

The flag is read from an env var at launch (no rebuild needed to flip it) — see the override
in `packages/react-native/ReactCommon/.../ReactNativeFeatureFlagsAccessor.cpp`
(`enableFabricCommitBranching()` returns `RNT_COMMIT_BRANCHING` when set).

```sh
# from packages/rn-tester
yarn start --port 8088                 # Metro (8081 may be taken; pick anything)

# build for the booted simulator
UDID=$(xcrun simctl list devices booted | grep -oE '[0-9A-F-]{36}' | head -1)
xcodebuild -workspace RNTesterPods.xcworkspace -scheme RNTester \
  -configuration Debug -sdk iphonesimulator -destination "id=$UDID" \
  ONLY_ACTIVE_ARCH=YES build

APP=$(find ~/Library/Developer/Xcode/DerivedData/RNTesterPods-*/Build/Products/Debug-iphonesimulator -name RNTester.app | head -1)
xcrun simctl install $UDID "$APP"
xcrun simctl spawn $UDID defaults write com.meta.RNTester.localDevelopment RCT_jsLocation -string "localhost:8088"

# launch with the flag ON or OFF (the in-app badge shows the actual value)
SIMCTL_CHILD_RNT_COMMIT_BRANCHING=1 xcrun simctl launch $UDID com.meta.RNTester.localDevelopment   # ON
SIMCTL_CHILD_RNT_COMMIT_BRANCHING=0 xcrun simctl launch $UDID com.meta.RNTester.localDevelopment   # OFF
```

Watch the measurements in the device log:

```sh
xcrun simctl spawn $UDID log stream --style compact \
  --predicate 'eventMessage CONTAINS "RNTMount" OR eventMessage CONTAINS "RNTProbe" OR eventMessage CONTAINS "RNTStarve"'
```

### Using the screen

- **Jank** (`startStarve`): installs a cheap, self-re-signaling `source0` on the main run
  loop. It keeps the loop in `poll == true` so it never reaches its sleep point and
  `kCFRunLoopBeforeWaiting` never fires. It is *non-blocking* (no busy-wait), so the main GCD
  queue and touch delivery stay responsive — only `BeforeWaiting`-driven work is starved.
- **Update component**: one React state update. Logs the latency split (see below).
- **Auto-update**: drives the same update from a 16 ms JS `setInterval`. No touch involved,
  so it isolates the commit→mount path. The log lines `[RNTMount] nativeID=probe-N` count
  actual mounts.

### What you'll measure (jank ON)

| | flag OFF | flag ON |
|---|---|---|
| **Update** — `timeToMount` (commit→mount) | ~6 ms (dispatch port) | ~one cycle (merge waits for `BeforeWaiting`) |
| **Auto-update** — mounts/sec | ~60/s, `probe-N` increments by 1 | ~`BeforeWaiting` rate (≈1–2/s), `probe-N` jumps by ~36 (coalesced) |

---

## 2. How the regression happens

After [#56726], a React-branch commit's **merge** is drained from a
`kCFRunLoopBeforeWaiting` run-loop observer (`RCTSurfacePresenter.mm`,
`ReactRevisionMergeRunLoopObserverDelegate`). `BeforeWaiting` only fires when the main run
loop is about to go idle.

The "Jank" switch keeps the loop from ever going idle, so `BeforeWaiting` is suppressed:

```
flag OFF:  commit ──────────────► mount (via main-queue dispatch port, serviced every loop turn)
flag ON:   commit ─► React branch ─► merge (waits for kCFRunLoopBeforeWaiting) ─► mount
                                       └── starved while the loop never sleeps ──┘
```

So with branching ON, a React-driven update is throttled to the `BeforeWaiting` rate, and
intermediate revisions are **coalesced** (only the latest merges each time). Without
branching the commit mounts directly through the main-queue dispatch port, which is serviced
on every loop turn regardless of `BeforeWaiting`. → **branching adds one extra
`BeforeWaiting` cycle per React update**, and caps throughput at the `BeforeWaiting` rate.

### Why the per-tap latency is muddier than the throughput

The **input path** has the *same* dependency: iOS delivers native events to JS via the
`AppleEventBeat`, which is **also** a `kCFRunLoopBeforeWaiting` observer
(`RCTSurfacePresenter.mm`, `AppleEventBeat`). So under this stimulus a touch takes ~one cycle
to even reach the JS handler — for *both* flags. That's why the per-tap split is logged:

```
[RNTProbe] tapToHandler=…  touchToJs=…  jsToMain=…   # touchToJs = event-beat tax (both flags)
[RNTMount] nativeID=probe-N … timeToMount=…          # timeToMount = merge tax (flag ON only)
```

- `touchToJs` ≈ one cycle (event beat, **both** flags — shared input tax)
- `jsToMain` ≈ 0 (main GCD queue is *not* starved — it drains every loop turn)
- `timeToMount` ≈ one extra cycle with the flag **ON** only (the merge)

The **Auto-update / throughput** mode avoids the event beat entirely (updates come from a JS
timer, not a touch), so it's the cleanest demonstration: the flag is the *only* difference.

> Note: this is a synthetic "never-sleep" stimulus. A real workload that keeps the main
> thread busy but still reaches `BeforeWaiting` intermittently would show the same merge
> being one `BeforeWaiting` interval behind, just less extreme.

### What's native vs JS

- **JS** (`CommitBranchingExample.js`): the whole example UI and the two drive modes.
- **`AppDelegate.mm` / `RNTDemo`**: the `source0` jank (`startStarve`/`stopStarve`), a
  swizzled `UIApplication sendEvent:` to timestamp touch-down, and `markPress`. `RNTJS`
  (background queue) timestamps when the JS handler ran, to split `touchToJs` vs `jsToMain`.
- **`RCTMountingManager.mm`**: logs `[RNTMount] nativeID=probe-N … timeToMount` on each mount.
- **`RCTSurfacePresenter.mm`**: `[RNTMerge]` logs the merge draining at `BeforeWaiting`.

[#56726]: https://github.com/facebook/react-native/pull/56726
