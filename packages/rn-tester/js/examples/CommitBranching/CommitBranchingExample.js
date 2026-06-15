/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

'use strict';

const React = require('react');
const {useEffect, useState} = React;
const {Button, NativeModules, StyleSheet, Switch, Text, View} =
  require('react-native');

// Native demo harness (RNTester/AppDelegate.mm):
//   blockMainThread(ms) — sleeps the MAIN thread for `ms` (one heavy transition frame).
//   mark(label)         — logs a timestamp so the native [RNTProbe] MOUNT-TX log can
//                         measure navigate -> first-content-mount (TTRC).
// blockMainThread has to be native: JS runs on its own thread and cannot occupy the
// main thread. Everything else here is plain JS.
const RNTDemo = NativeModules.RNTDemo;

/*
 * enableFabricCommitBranching TTRC regression — minimal repro.
 *
 * The new screen's content is a React commit. With the flag ON it does NOT mount
 * inline; it is deferred to the React branch and only mounts after an extra hop:
 *
 *     commit -> React branch -> RuntimeScheduler promotion (JS)
 *            -> RCTExecuteOnMainQueue(merge)  <-- extra async hop, main queue
 *            -> mount
 *
 * That extra `RCTExecuteOnMainQueue` merge (RCTSurfacePresenter.mm) is a second
 * main-thread round-trip. When a tab/stack transition keeps the main thread busy with
 * long synchronous frames, the merge is queued behind that work, so the incoming
 * screen's first content mounts ~one extra transition-frame late.
 *
 * Repro: tap "Navigate" with "Simulate janky transition" ON.
 *   - Flag OFF: new content appears quickly.
 *   - Flag ON : new content appears noticeably later (it waits for the merge hop to
 *               get through the transition's busy frames).
 * Watch the [RNTProbe] device log: NAVIGATE -> MOUNT-TX is the measured TTRC.
 *
 * Flip the flag at launch (no rebuild):
 *   xcrun simctl terminate <udid> com.meta.RNTester.localDevelopment
 *   SIMCTL_CHILD_RNT_COMMIT_BRANCHING=1 xcrun simctl launch <udid> com.meta.RNTester.localDevelopment  # ON
 *   SIMCTL_CHILD_RNT_COMMIT_BRANCHING=0 xcrun simctl launch <udid> com.meta.RNTester.localDevelopment  # OFF
 */

const BLOCK_MS = 300; // length of each synchronous "transition frame"
const TRANSITION_MS = 1500; // how long the janky transition runs

// A JS loop of main-thread blocks = repeated long busy frames with brief gaps in
// between (the shape that delays the branch merge hop).
function runJankyTransition() {
  if (RNTDemo == null) {
    return;
  }
  const end = Date.now() + TRANSITION_MS;
  const step = () => {
    if (Date.now() >= end) {
      return;
    }
    RNTDemo.blockMainThread(BLOCK_MS); // freezes the main thread for one "frame"
    setTimeout(step, 0); // yield, then block again
  };
  step();
}

function CommitBranchingTTRC() /*: React.Node */ {
  const [jank, setJank] = useState(true);
  const [screen, setScreen] = useState(0); // 0 = nothing navigated yet

  // Per-frame animation: one setState per requestAnimationFrame == one React commit per
  // frame == (when not starved) one real mount per frame. Used with startStarve below to
  // ask: does suppressing BeforeWaiting for several frames collapse the *mounts*?
  const [anim, setAnim] = useState(false);
  const [tick, setTick] = useState(0);
  useEffect(() => {
    if (!anim) {
      return;
    }
    let raf = 0;
    let cancelled = false;
    let n = 0;
    const loop = () => {
      if (cancelled) {
        return;
      }
      n += 1;
      globalThis.__rafCount = n; // read via debugger to get the real commit rate
      setTick(n);
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => {
      cancelled = true;
      cancelAnimationFrame(raf);
    };
  }, [anim]);

  const navigate = () => {
    RNTDemo && RNTDemo.mark('NAVIGATE');
    setScreen(n => n + 1); // mount brand-new content (a React commit)
    if (jank) {
      runJankyTransition(); // ...while a heavy transition hogs the main thread
    }
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>Commit Branching — TTRC</Text>
      <Text style={styles.blurb}>
        Tap Navigate with the transition ON. With enableFabricCommitBranching ON the new
        screen's content appears ~one transition-frame late (its branch merge is queued
        behind the main-thread work); with the flag OFF it appears promptly. The
        [RNTProbe] log prints NAVIGATE → MOUNT-TX = the TTRC.
      </Text>

      <View style={styles.row}>
        <Text style={styles.rowLabel}>Simulate janky transition</Text>
        <Switch value={jank} onValueChange={setJank} />
      </View>

      <View style={styles.row}>
        <Text style={styles.rowLabel}>Run rAF animation (tick {tick})</Text>
        <Switch value={anim} onValueChange={setAnim} />
      </View>

      <Button title="Navigate to new screen" onPress={navigate} />

      <View style={styles.stage}>
        {screen === 0 ? (
          <Text style={styles.placeholder}>no screen yet — tap Navigate</Text>
        ) : (
          <View style={styles.screen} testID="ttrc-screen">
            <Text style={styles.screenTitle}>SCREEN #{screen}</Text>
            <Text style={styles.screenBody}>first content — this is what TTRC measures</Text>
          </View>
        )}
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {flex: 1, padding: 16},
  title: {fontSize: 22, fontWeight: '800', marginBottom: 8},
  blurb: {fontSize: 13, color: '#555', marginBottom: 16, lineHeight: 18},
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    marginBottom: 12,
  },
  rowLabel: {fontSize: 15},
  stage: {flex: 1, marginTop: 20, justifyContent: 'center'},
  placeholder: {textAlign: 'center', color: '#999'},
  screen: {
    padding: 24,
    borderRadius: 14,
    backgroundColor: '#f59e0b',
    alignItems: 'center',
  },
  screenTitle: {fontSize: 28, fontWeight: '900', color: '#fff'},
  screenBody: {fontSize: 14, color: '#fff', marginTop: 6},
});

exports.title = 'Commit Branching TTRC';
exports.category = 'UI';
exports.description =
  'enableFabricCommitBranching delays a new screen’s first content (TTRC) when a ' +
  'transition keeps the main thread busy. Toggle the flag via RNT_COMMIT_BRANCHING.';
exports.displayName = 'CommitBranchingExample';
exports.examples = [
  {
    title: 'TTRC under a janky transition',
    name: 'ttrc',
    render(): React.Node {
      return <CommitBranchingTTRC />;
    },
  },
];
