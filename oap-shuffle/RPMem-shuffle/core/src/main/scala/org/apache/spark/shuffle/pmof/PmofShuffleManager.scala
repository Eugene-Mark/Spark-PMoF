package org.apache.spark.shuffle.pmof

import java.util.concurrent.ConcurrentHashMap

import org.apache.spark.internal.Logging
import org.apache.spark.network.pmof.PmofTransferService
import org.apache.spark.shuffle._
import org.apache.spark.util.configuration.pmof.PmofConf
import org.apache.spark.{ShuffleDependency, SparkConf, SparkEnv, TaskContext}

private[spark] class PmofShuffleManager(conf: SparkConf) extends ShuffleManager with Logging {
  logInfo("Initialize RdmaShuffleManager")

  if (!conf.getBoolean("spark.shuffle.spill", defaultValue = true)) {
    logWarning("spark.shuffle.spill was set to false")
  }

  private[this] val numMapsForShuffle = new ConcurrentHashMap[Int, Int]()
  private[this] val pmofConf = new PmofConf(conf)
  var metadataResolver: MetadataResolver = _

  override def registerShuffle[K, V, C](shuffleId: Int, dependency: ShuffleDependency[K, V, C]): ShuffleHandle = {
    val env: SparkEnv = SparkEnv.get

    metadataResolver = MetadataResolver.getMetadataResolver(pmofConf)

    if (pmofConf.enableRdma) {
      PmofTransferService.getTransferServiceInstance(pmofConf: PmofConf, env.blockManager, this, isDriver = true)
    }

    new BaseShuffleHandle(shuffleId, dependency)
  }

  override def getWriter[K, V](handle: ShuffleHandle, mapId: Long, context: TaskContext, metrics: ShuffleWriteMetricsReporter): ShuffleWriter[K, V] = {
    assert(handle.isInstanceOf[BaseShuffleHandle[_, _, _]])

    val env: SparkEnv = SparkEnv.get
    val blockManager = SparkEnv.get.blockManager
    val serializerManager = SparkEnv.get.serializerManager

    metadataResolver = MetadataResolver.getMetadataResolver(pmofConf)
    //numMapsForShuffle.putIfAbsent(handle.shuffleId, numMaps)

    if (pmofConf.enableRdma) {
      PmofTransferService.getTransferServiceInstance(pmofConf, env.blockManager, this)
    }

    if (pmofConf.enablePmem) {
      new PmemShuffleWriter(shuffleBlockResolver.asInstanceOf[PmemShuffleBlockResolver], metadataResolver, blockManager, serializerManager, 
        handle.asInstanceOf[BaseShuffleHandle[K, V, _]], mapId, context, env.conf, pmofConf)
    } else {
      new BaseShuffleWriter(shuffleBlockResolver.asInstanceOf[IndexShuffleBlockResolver], metadataResolver, blockManager, serializerManager, 
        handle.asInstanceOf[BaseShuffleHandle[K, V, _]], mapId, context, pmofConf)
    }
  }

  override def getReader[K, C](handle: _root_.org.apache.spark.shuffle.ShuffleHandle, startPartition: Int, endPartition: Int, context: _root_.org.apache.spark.TaskContext, metricsReporter: ShuffleReadMetricsReporter): _root_.org.apache.spark.shuffle.ShuffleReader[K, C] = {
    if (pmofConf.enableRdma) {
      new RdmaShuffleReader(handle.asInstanceOf[BaseShuffleHandle[K, _, C]],
        startPartition, endPartition, context, pmofConf)
    } else {
      val tempMetrics = context.taskMetrics().createTempShuffleReadMetrics()
      new BaseShuffleReader(
        handle.asInstanceOf[BaseShuffleHandle[K, _, C]], startPartition, endPartition, context, pmofConf)
    }
  }

  override def unregisterShuffle(shuffleId: Int): Boolean = {
    Option(numMapsForShuffle.remove(shuffleId)).foreach { numMaps =>
      (0 until numMaps).foreach { mapId =>
        shuffleBlockResolver.asInstanceOf[IndexShuffleBlockResolver].removeDataByMap(shuffleId, mapId)
      }
    }
    true
  }

  override def stop(): Unit = {
    shuffleBlockResolver.stop()
  }

  override val shuffleBlockResolver: ShuffleBlockResolver = {
    if (pmofConf.enablePmem)
      new PmemShuffleBlockResolver(conf)
    else
      new IndexShuffleBlockResolver(conf)
  }

  override def getReaderForOneMapper[K, C](handle: ShuffleHandle, mapIndex: Int, startPartition: Int, endPartition: Int, context: TaskContext, metrics: ShuffleReadMetricsReporter): ShuffleReader[K, C] = ???
}
