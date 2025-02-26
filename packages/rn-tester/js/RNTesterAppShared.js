/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @format
 * @flow
 */

import type {RNTesterModuleInfo, ScreenTypes} from './types/RNTesterTypes';

import RNTesterModuleContainer from './components/RNTesterModuleContainer';
import RNTesterModuleList from './components/RNTesterModuleList';
import RNTesterNavBar, {navBarHeight} from './components/RNTesterNavbar';
import {RNTesterThemeContext, themes} from './components/RNTesterTheme';
import RNTTitleBar from './components/RNTTitleBar';
import {title as PlaygroundTitle} from './examples/Playground/PlaygroundExample';
import RNTesterList from './utils/RNTesterList';
import {
  RNTesterNavigationActionsType,
  RNTesterNavigationReducer,
} from './utils/RNTesterNavigationReducer';
import {
  Screens,
  getExamplesListWithRecentlyUsed,
  initialNavigationState,
} from './utils/testerStateUtils';
import * as React from 'react';
import {useEffect} from 'react';
import {
  BackHandler,
  Button,
  Linking,
  Platform,
  StyleSheet,
  View,
  useColorScheme,
} from 'react-native';
import * as NativeComponentRegistry from 'react-native/Libraries/NativeComponent/NativeComponentRegistry';
import {
  SampleNativeComponent,
  SampleNativeComponentCommands,
} from '@react-native/oss-library-example';

// In Bridgeless mode, in dev, enable static view config validator
if (global.RN$Bridgeless === true && __DEV__) {
  NativeComponentRegistry.setRuntimeConfigProvider(() => {
    return {
      native: false,
      verify: true,
    };
  });
}

// RNTester App currently uses in memory storage for storing navigation state

type BackButton = ({onBack: () => void}) => React.Node;

const RNTesterApp = ({
  testList,
  customBackButton,
}: {
  testList?: {
    components?: Array<RNTesterModuleInfo>,
    apis?: Array<RNTesterModuleInfo>,
  },
  customBackButton?: BackButton,
}): React.Node => {
  const ref = React.useRef();

  useEffect(() => {
    const t = setTimeout(() => {
      SampleNativeComponentCommands.changeBackgroundColor(
        // $FlowFixMe[incompatible-call]
        ref.current,
        '#0000ff',
      );
    }, 5000);

    () => clearTimeout(t);
  }, []);

  return (
    <View style={styles.container}>
      {/* <View style={{flex: 1}}> */}
      <SampleNativeComponent ref={ref} style={{flex: 1}} opacity={0.5} />
      {/* </View> */}
      <View nativeID="clone-me" style={{backgroundColor: 'green', flex: 1}} />
    </View>
  );
};

export default RNTesterApp;

const styles = StyleSheet.create({
  container: {
    flex: 1,
  },
  bottomNavbar: {
    height: navBarHeight,
  },
  hidden: {
    display: 'none',
  },
});
