/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

@file:Suppress("DEPRECATION")

package com.facebook.react.fabric

import com.facebook.react.ReactRootView
import com.facebook.react.bridge.JavaOnlyMap
import com.facebook.react.bridge.ReactTestHelper
import com.facebook.react.fabric.mounting.MountingManager
import com.facebook.react.fabric.mounting.SurfaceMountingManager
import com.facebook.react.fabric.mounting.mountitems.BatchedAnimatedPropsMountItem
import com.facebook.react.fabric.mounting.mountitems.IntBufferBatchMountItem
import com.facebook.react.internal.featureflags.ReactNativeFeatureFlagsForTests
import com.facebook.react.uimanager.ThemedReactContext
import com.facebook.react.uimanager.ViewManager
import com.facebook.react.uimanager.ViewManagerRegistry
import com.facebook.react.views.view.ReactViewManager
import com.facebook.testutils.shadows.ShadowNativeLoader
import com.facebook.testutils.shadows.ShadowNativeMap
import com.facebook.testutils.shadows.ShadowReadableNativeArray
import com.facebook.testutils.shadows.ShadowReadableNativeMap
import com.facebook.testutils.shadows.ShadowSoLoader
import com.facebook.testutils.shadows.ShadowWritableNativeArray
import com.facebook.testutils.shadows.ShadowWritableNativeMap
import org.assertj.core.api.Assertions.assertThat
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(
    shadows =
        [
            ShadowSoLoader::class,
            ShadowNativeLoader::class,
            ShadowNativeMap::class,
            ShadowWritableNativeMap::class,
            ShadowReadableNativeMap::class,
            ShadowWritableNativeArray::class,
            ShadowReadableNativeArray::class,
        ],
)
class MountingManagerAnimatedPropsTest {
  private lateinit var mountingManager: MountingManager
  private lateinit var themedReactContext: ThemedReactContext
  private val surfaceId = 1

  @Before
  fun setUp() {
    ReactNativeFeatureFlagsForTests.setUp()
    val reactContext = ReactTestHelper.createCatalystContextForTest()
    themedReactContext = ThemedReactContext(reactContext, reactContext, null, -1)
    mountingManager =
        MountingManager(ViewManagerRegistry(listOf<ViewManager<*, *>>(ReactViewManager())), {})
  }

  private fun startSurfaceWithView(tag: Int): SurfaceMountingManager {
    mountingManager.startSurface(surfaceId, themedReactContext, ReactRootView(themedReactContext))
    val smm = mountingManager.getSurfaceManagerEnforced(surfaceId, "test")
    smm.preallocateView("RCTView", tag, JavaOnlyMap.of(), null, true)
    smm.addViewAt(surfaceId, tag, 0)
    return smm
  }

  private fun execute(intBuffer: IntArray, objBuffer: Array<Any?>, directWrites: Array<Any?>?) {
    IntBufferBatchMountItem(surfaceId, intBuffer, objBuffer, 0) { directWrites }
        .execute(mountingManager)
  }

  @Test
  fun backendWritesDoNotPinLaterMounts() {
    val smm = startSurfaceWithView(42)
    BatchedAnimatedPropsMountItem(
            intArrayOf(CMD_START_OF_VIEW, 42, CMD_RAW_PROPS, CMD_END_OF_VIEW),
            doubleArrayOf(),
            arrayOf(JavaOnlyMap.of("opacity", 0.3)),
        )
        .execute(mountingManager)
    assertThat(smm.getView(42).alpha).isEqualTo(0.3f)

    smm.updateProps(42, JavaOnlyMap.of("opacity", 1.0))

    assertThat(smm.getView(42).alpha).isEqualTo(1.0f)
  }

  @Test
  fun backendWritesToMissingViewsAreSkipped() {
    startSurfaceWithView(42)

    BatchedAnimatedPropsMountItem(
            intArrayOf(CMD_START_OF_VIEW, 7, CMD_RAW_PROPS, CMD_END_OF_VIEW),
            doubleArrayOf(),
            arrayOf(JavaOnlyMap.of("opacity", 0.3)),
        )
        .execute(mountingManager)

    assertThat(mountingManager.getViewExists(7)).isFalse()
  }

  @Test
  fun updateKeepsNewerDirectWritesOfItsKeys() {
    val smm = startSurfaceWithView(42)
    smm.updatePropsSynchronously(42, JavaOnlyMap.of("translateY", 40.0))

    execute(
        intArrayOf(IntBufferBatchMountItem.INSTRUCTION_UPDATE_PROPS, 42),
        arrayOf(JavaOnlyMap.of("opacity", 1.0)),
        arrayOf(JavaOnlyMap.of("opacity", 0.3, "translateY", 10.0)),
    )

    assertThat(smm.getView(42).alpha).isEqualTo(0.3f)
    assertThat(smm.getView(42).translationY).isEqualTo(40.0f)
  }

  @Test
  fun updateWithoutNewerDirectWritesAppliesItsProps() {
    val smm = startSurfaceWithView(42)

    execute(
        intArrayOf(IntBufferBatchMountItem.INSTRUCTION_UPDATE_PROPS, 42),
        arrayOf(JavaOnlyMap.of("opacity", 0.6)),
        arrayOf(null),
    )

    assertThat(smm.getView(42).alpha).isEqualTo(0.6f)
  }

  @Test
  fun createKeepsNewerDirectWritesOfItsKeys() {
    mountingManager.startSurface(surfaceId, themedReactContext, ReactRootView(themedReactContext))
    val smm = mountingManager.getSurfaceManagerEnforced(surfaceId, "test")

    execute(
        intArrayOf(IntBufferBatchMountItem.INSTRUCTION_CREATE, 42, 1),
        arrayOf("View", JavaOnlyMap.of("opacity", 1.0), null, null),
        arrayOf(JavaOnlyMap.of("opacity", 0.3, "translateY", 10.0)),
    )

    assertThat(smm.getView(42).alpha).isEqualTo(0.3f)
    assertThat(smm.getView(42).translationY).isEqualTo(0.0f)
  }

  @Test
  fun insertAppliesEveryNewerDirectWrite() {
    mountingManager.startSurface(surfaceId, themedReactContext, ReactRootView(themedReactContext))
    val smm = mountingManager.getSurfaceManagerEnforced(surfaceId, "test")
    smm.preallocateView("RCTView", 42, JavaOnlyMap.of(), null, true)

    execute(
        intArrayOf(IntBufferBatchMountItem.INSTRUCTION_INSERT, 42, surfaceId, 0),
        arrayOf(),
        arrayOf(JavaOnlyMap.of("opacity", 0.3)),
    )

    assertThat(smm.getView(42).alpha).isEqualTo(0.3f)
  }

  private companion object {
    const val CMD_START_OF_VIEW = 1
    const val CMD_END_OF_VIEW = 4
    const val CMD_RAW_PROPS = 5
  }
}
