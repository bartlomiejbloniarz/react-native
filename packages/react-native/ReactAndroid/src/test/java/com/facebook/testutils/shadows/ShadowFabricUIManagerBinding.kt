/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.testutils.shadows

import com.facebook.react.fabric.FabricUIManagerBinding
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements

/**
 * [FabricUIManagerBinding] is JNI-backed and Mockito can't mock native methods, so shadow the
 * methods tests need instead.
 */
@Implements(FabricUIManagerBinding::class)
class ShadowFabricUIManagerBinding {
  val pulledSurfaceIds: MutableList<Int> = mutableListOf()

  @Implementation
  fun pullAndExecuteTransaction(surfaceId: Int) {
    pulledSurfaceIds.add(surfaceId)
  }
}
