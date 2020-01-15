package net.corda.core.flows

import java.util.concurrent.CompletableFuture

/**
 * [FlowExternalFuture] represents an external future that blocks a flow from continuing until the future returned by
 * [FlowExternalFuture.execute] has completed. Examples of external processes where [FlowExternalFuture] would be useful include,
 * triggering a long running process on an external system or retrieving information from a service that might be down.
 *
 * The flow will suspend while it is blocked to free up a flow worker thread, which allows other flows to continue processing while waiting
 * for the result of this process.
 *
 * Implementations of [FlowExternalFuture] should ideally hold references to any external values required by [execute]. These references
 * should be passed into the implementation's constructor. For example, an amount or a reference to a Corda Service could be passed in.
 *
 * It is discouraged to insert into the node's database from a [FlowExternalFuture], except for keeping track of [deduplicationId]s that
 * have been processed. It is possible to interact with the database from inside a [FlowExternalFuture] but, for most operations, is not
 * currently supported.
 */
interface FlowExternalFuture<R : Any> {

    /**
     * Executes a future.
     *
     * The future created and returned from [execute] must handle their own threads. If a new thread is not spawned or taken from a thread
     * pool, then the flow worker thread will be used. This removes any benefit from using an [FlowExternalFuture].
     *
     * @param deduplicationId  If the flow restarts from a checkpoint (due to node restart, or via a visit to the flow
     * hospital following an error) the execute method might be called more than once by the Corda flow state machine.
     * For each duplicate call, the deduplicationId is guaranteed to be the same allowing duplicate requests to be
     * de-duplicated if necessary inside the execute method.
     */
    fun execute(deduplicationId: String): CompletableFuture<R>
}

/**
 * [FlowExternalResult] represents an external process that blocks a flow from continuing until the result of [execute]
 * has been retrieved. Examples of external processes where [FlowExternalResult] would be useful include, triggering a long running process
 * on an external system or retrieving information from a service that might be down.
 *
 * The flow will suspend while it is blocked to free up a flow worker thread, which allows other flows to continue processing while waiting
 * for the result of this process.
 *
 * Implementations of [FlowExternalResult] should ideally hold references to any external values required by [execute]. These references
 * should be passed into the implementation's constructor. For example, an amount or a reference to a Corda Service could be passed in.
 *
 * It is discouraged to insert into the node's database from a [FlowExternalResult], except for keeping track of [deduplicationId]s that
 * have been processed. It is possible to interact with the database from inside a [FlowExternalResult] but, for most operations, is not
 * currently supported.
 */
interface FlowExternalResult<R : Any> {

    /**
     * Executes a blocking operation.
     *
     * The execution of [execute] will be run on a thread from the node's external process thread pool when called by [FlowLogic.await].
     *
     * @param deduplicationId  If the flow restarts from a checkpoint (due to node restart, or via a visit to the flow
     * hospital following an error) the execute method might be called more than once by the Corda flow state machine.
     * For each duplicate call, the deduplicationId is guaranteed to be the same allowing duplicate requests to be
     * de-duplicated if necessary inside the execute method.
     */
    fun execute(deduplicationId: String): R
}