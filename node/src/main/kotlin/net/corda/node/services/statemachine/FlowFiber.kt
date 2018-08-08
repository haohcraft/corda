/*
 * R3 Proprietary and Confidential
 *
 * Copyright (c) 2018 R3 Limited.  All rights reserved.
 *
 * The intellectual and technical concepts contained herein are proprietary to R3 and its suppliers and are protected by trade secret law.
 *
 * Distribution of this file or any portion thereof via any medium without the express permission of R3 is strictly prohibited.
 */

package net.corda.node.services.statemachine

import co.paralleluniverse.fibers.Suspendable
import net.corda.core.flows.StateMachineRunId
import net.corda.node.services.statemachine.transitions.StateMachine

/**
 * An interface wrapping a fiber running a flow.
 */
interface FlowFiber {
    val id: StateMachineRunId
    val stateMachine: StateMachine

    @Suspendable
    fun scheduleEvent(event: Event)

    fun snapshot(): StateMachineState
}