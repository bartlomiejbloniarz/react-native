# Commit Branching TTRC regression — minimal repro

`enableFabricCommitBranching` delays a new screen's **first content (TTRC)** when a
transition keeps the main thread busy. This is the smallest repro: one RNTester screen,
one button, one toggle.

## The failure in one sentence

With the flag ON, a new screen's content does **not** mount inline — it is deferred to the
React branch and only mounts after an extra `RCTExecuteOnMainQueue` "merge" hop, which gets
**queued behind the transition's main-thread work**, so the content lands ~one transition
frame late.

```
flag ON:   commit ─► React branch ─► RuntimeScheduler promotion (JS thread)
                  ─► RCTExecuteOnMainQueue(merge)   ← extra hop, waits behind the busy main thread
                  ─► mount
flag OFF:  commit ─► mount (inline, before the transition's first busy frame runs)
```

## Run it

RNTester → **APIs** tab → **Commit Branching TTRC**. Leave "Simulate janky transition" ON,
tap **Navigate to new screen**, and watch when `SCREEN #n` appears.

Flip the flag at launch (no rebuild — read from env in `ReactNativeFeatureFlagsAccessor.cpp`):

```sh
UDID=<your sim udid>
xcrun simctl terminate $UDID com.meta.RNTester.localDevelopment
SIMCTL_CHILD_RNT_COMMIT_BRANCHING=1 xcrun simctl launch $UDID com.meta.RNTester.localDevelopment  # ON
SIMCTL_CHILD_RNT_COMMIT_BRANCHING=0 xcrun simctl launch $UDID com.meta.RNTester.localDevelopment  # OFF
```

Measured TTRC (`NAVIGATE → MOUNT-TX` from the device log, `log stream --predicate
'eventMessage CONTAINS "RNTProbe"'`):

| | transition OFF | transition ON |
|---|---|---|
| **flag ON** (branching) | ~8 ms | **~300 ms** |
| **flag OFF** | ~8 ms | **~10 ms** |

## What is / isn't native

- **JS** (`CommitBranchingExample.js`): the whole repro — the Navigate action and the janky
  transition (a JS loop). This is what you read to understand the bug.
- **C++ flag**: `enableFabricCommitBranching` (default flipped on; env-overridable for A/B).
- **Native primitive** (`AppDelegate.mm`, `RNTDemo`): `blockMainThread(ms)` — a `usleep` on
  the main thread. It *has* to be native: JS runs on its own thread and cannot occupy the
  main thread (which is the whole point — a real transition's heavy frames do). `mark(label)`
  just logs a timestamp.
- **Measurement** (`RCTSurfacePresenter.mm`): `[RNTProbe] MOUNT-TX` logs every mount, and
  `[RNTMerge]` logs the React-branch merge's main-queue wait.

## Why the transition has to block the main thread continuously

A smooth animation (short per-frame work with gaps) does *not* trigger this — the merge hop
slips into the gaps. The regression needs **long synchronous main-thread frames** (a heavy
transition), which is what `blockMainThread` simulates. With branching OFF the content mounts
inline before the first such frame; with branching ON the merge hop lands *after* it.
