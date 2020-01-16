package net.corda.node.services

import co.paralleluniverse.fibers.Suspendable
import net.corda.core.flows.FlowLogic
import net.corda.core.flows.StartableByService
import net.corda.core.identity.Party
import net.corda.core.node.AppServiceHub
import net.corda.core.node.services.CordaService
import net.corda.core.node.services.ServiceLifecycleEvent
import net.corda.core.serialization.SingletonSerializeAsToken
import net.corda.core.utilities.OpaqueBytes
import net.corda.core.utilities.getOrThrow
import net.corda.finance.DOLLARS
import net.corda.finance.flows.AbstractCashFlow
import net.corda.finance.flows.CashIssueAndPaymentFlow
import net.corda.testing.driver.DriverParameters
import net.corda.testing.driver.driver
import net.corda.testing.node.internal.FINANCE_CORDAPPS
import net.corda.testing.node.internal.enclosedCordapp
import org.junit.Test
import kotlin.test.assertNotNull

/**
 * The idea of this test is upon start-up of the node check if cash been already issued and if not issue under certain reference.
 * If state is already present - do nothing.
 */
class CordaServiceIssueOnceAtStartupTests {
    @Test
    fun test() {
        driver(DriverParameters(startNodesInProcess = true, cordappsForAllNodes = FINANCE_CORDAPPS + enclosedCordapp())) {
            val node = startNode().getOrThrow()
        }
    }

    @CordaService
    class IssueAndPayOnceService(private val services: AppServiceHub) : SingletonSerializeAsToken() {

        init {
            services.register(func = this::handleEvent)
        }

        private fun handleEvent(event: ServiceLifecycleEvent) {

            when (event) {
                ServiceLifecycleEvent.CORDAPP_STARTED -> {
                    assertNotNull(services.startFlow(
                            IssueAndPayByServiceFlow(services.myInfo.legalIdentities.single(), services.networkMapCache.notaryIdentities.single())).
                            returnValue.getOrThrow())
                }
                else -> {
                    // Do nothing
                }
            }
        }
    }

    /**
     * The only purpose to have this is to be able to have annotation: [StartableByService]
     */
    @StartableByService
    class IssueAndPayByServiceFlow(private val recipient: Party, private val notary: Party) : FlowLogic<AbstractCashFlow.Result>() {
        @Suspendable
        override fun call(): AbstractCashFlow.Result {
            return subFlow(CashIssueAndPaymentFlow(500.DOLLARS,
                    OpaqueBytes.of(0x01),
                    recipient,
                    false,
                    notary))
        }
    }
}