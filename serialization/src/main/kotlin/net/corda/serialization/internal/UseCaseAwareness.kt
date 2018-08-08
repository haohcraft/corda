/*
 * R3 Proprietary and Confidential
 *
 * Copyright (c) 2018 R3 Limited.  All rights reserved.
 *
 * The intellectual and technical concepts contained herein are proprietary to R3 and its suppliers and are protected by trade secret law.
 *
 * Distribution of this file or any portion thereof via any medium without the express permission of R3 is strictly prohibited.
 */

@file:KeepForDJVM
package net.corda.serialization.internal

import net.corda.core.KeepForDJVM
import net.corda.core.serialization.SerializationContext
import net.corda.core.serialization.SerializationFactory
import java.util.*

fun checkUseCase(allowedUseCases: EnumSet<SerializationContext.UseCase>) {
    val currentContext: SerializationContext = SerializationFactory.currentFactory?.currentContext
            ?: throw IllegalStateException("Current context is not set")
    if (!allowedUseCases.contains(currentContext.useCase)) {
        throw IllegalStateException("UseCase '${currentContext.useCase}' is not within '$allowedUseCases'")
    }
}