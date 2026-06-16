/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 */

// @flow
'use strict';

const React = require('react');
const {
  Button,
  NativeModules,
  StyleSheet,
  Switch,
  Text,
  View,
} = require('react-native');

const {useEffect, useRef, useState} = React;

const RNTDemo = NativeModules.RNTDemo;
const RNTJS = NativeModules.RNTJS;

/*
 * enableFabricCommitBranching jank — minimal repro.
 *
 * Since #56726, a React-branch commit's merge is drained from a kCFRunLoopBeforeWaiting
 * main run loop observer (RCTSurfacePresenter). The "Jank" switch installs a source0 that
 * re-signals itself every iteration so the run loop stays in poll==true and never reaches
 * its sleep point, so BeforeWaiting never fires (for ~one cycle, then it re-arms).
 *
 *   flag ON : the component's merge waits for a BeforeWaiting that isn't coming → its
 *             mount is starved for ~one jank cycle (the box visibly lags each press).
 *   flag OFF: the component mounts via the main-queue dispatch port (serviced even while
 *             the loop never sleeps) → it updates immediately.
 *
 * IMPORTANT — the source0 perform is CHEAP (just re-signal + return); it does NOT busy-wait.
 * That is the whole point: a busy-waiting source0 also pins the main thread, so it delays
 * the *touch event and JS* too (you'd see lag even with the flag OFF). By returning to the
 * run loop on every iteration, input and the dispatch-port mount are still serviced between
 * re-signals — only the flag-ON BeforeWaiting merge is starved, so the flag is the *only*
 * difference you feel. (A single long sleep delays both flags equally; a CADisplayLink that
 * overruns the frame still SLEEPS until the next vsync so it does NOT reproduce.)
 *
 * Tap "Update component" with Jank on and watch the device log:
 *   [RNTMount] nativeID=probe-<n> mounted; timeToMount=<ms>
 *   flag ON ≈ hundreds of ms, flag OFF ≈ a few ms.
 */

function CommitBranchingJank() /*: React.Node */ {
  const [branching, setBranching] = useState(null /*: ?boolean */);
  const [jank, setJank] = useState(false);
  const [auto, setAuto] = useState(false);
  const [count, setCount] = useState(0);
  const intervalRef = useRef(null /*: ?IntervalID */);

  useEffect(() => {
    RNTDemo && RNTDemo.getBranching(v => setBranching(!!v));
    return () => {
      if (intervalRef.current != null) {
        clearInterval(intervalRef.current);
      }
    };
  }, []);

  // Drive updates from a JS interval — this NEVER touches the native->JS event beat (no
  // touch), so it isolates the commit->mount path. Throughput = how many of these actually
  // mount per second (count the [RNTMount] log lines): OFF keeps up with the interval, ON
  // collapses to the BeforeWaiting rate (~2/s) and coalesces, so the counter jumps in leaps.
  const toggleAuto = (on /*: boolean */) => {
    setAuto(on);
    if (intervalRef.current != null) {
      clearInterval(intervalRef.current);
      intervalRef.current = null;
    }
    if (on) {
      intervalRef.current = setInterval(() => setCount(c => c + 1), 16);
    }
  };

  const toggleJank = (on /*: boolean */) => {
    setJank(on);
    if (RNTDemo == null) {
      return;
    }
    // Cheap self-re-signaling source0 that keeps the loop in poll==true (no sleep) for a
    // ~600ms cycle (30 * 20ms), then re-arms. NOT busy-waiting — input stays responsive.
    on ? RNTDemo.startStarve(30, 20) : RNTDemo.stopStarve();
  };

  const update = () => {
    RNTJS && RNTJS.markJS(); // stamp when the JS handler ran (background queue, jank-free)
    RNTDemo && RNTDemo.markPress(); // stamp t0 (main queue) so native logs timeToMount + split
    setCount(c => c + 1);
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>Commit Branching jank</Text>

      <View
        style={[
          styles.badge,
          branching == null
            ? styles.badgeUnknown
            : branching
              ? styles.badgeOn
              : styles.badgeOff,
        ]}>
        <Text style={styles.badgeText}>
          enableFabricCommitBranching:{' '}
          {branching == null ? '…' : branching ? 'ON' : 'OFF'}
        </Text>
      </View>

      <Text style={styles.blurb}>
        Turn on Jank (a cheap self-re-signaling source0 keeps the run loop in
        poll==true so it never sleeps and BeforeWaiting never fires — but it does
        NOT busy-wait, so taps stay responsive), then tap Update. With branching
        ON the component's mount is starved ~one cycle (visible lag each press);
        with it OFF it updates right away. See the device log: [RNTMount]
        nativeID=probe-N timeToMount=…
      </Text>

      <View style={styles.row}>
        <Text style={styles.rowLabel}>Jank (never-sleep, non-blocking)</Text>
        <Switch value={jank} onValueChange={toggleJank} />
      </View>

      <View style={styles.row}>
        <Text style={styles.rowLabel}>Auto-update (interval, throughput)</Text>
        <Switch value={auto} onValueChange={toggleAuto} />
      </View>

      <Button title="Update component" onPress={update} />

      <View style={styles.stage}>
        <View nativeID={'probe-' + count} style={styles.probe}>
          <Text style={styles.probeText}>update #{count}</Text>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {flex: 1, padding: 16},
  title: {fontSize: 22, fontWeight: '800', marginBottom: 10},
  badge: {
    alignSelf: 'flex-start',
    paddingHorizontal: 10,
    paddingVertical: 5,
    borderRadius: 6,
  },
  badgeUnknown: {backgroundColor: '#999'},
  badgeOn: {backgroundColor: '#16a34a'},
  badgeOff: {backgroundColor: '#6b7280'},
  badgeText: {color: '#fff', fontWeight: '700', fontSize: 13},
  blurb: {
    fontSize: 13,
    color: '#555',
    marginTop: 12,
    marginBottom: 16,
    lineHeight: 18,
  },
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    marginBottom: 12,
  },
  rowLabel: {fontSize: 15},
  stage: {marginTop: 28, alignItems: 'center'},
  probe: {
    padding: 28,
    borderRadius: 14,
    backgroundColor: '#f59e0b',
    alignItems: 'center',
    minWidth: 200,
  },
  probeText: {fontSize: 28, fontWeight: '900', color: '#fff'},
});

exports.title = 'Commit Branching jank';
exports.category = 'UI';
exports.description =
  'enableFabricCommitBranching: under sustained over-budget frames (BeforeWaiting never ' +
  'fires) a component update is starved with the flag on, but not off.';
exports.displayName = 'CommitBranchingExample';
exports.examples = [
  {
    title: 'Update starved by jank (BeforeWaiting)',
    name: 'jank',
    render(): React.Node {
      return <CommitBranchingJank />;
    },
  },
];
