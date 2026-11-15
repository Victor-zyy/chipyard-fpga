package chipyard.fpga.zu19eg

import org.chipsalliance.cde.config.Config
import gemmini.CapacityInKilobytes

/** Independent FPGA experiment. The existing ZU19EG configurations are unchanged.
  * Keep one PE per tile: increase the pipelined mesh, not the combinational tile.
  * A dedicated generated header prevents overwriting the DIM=16 software ABI.
  * This changes geometry/capacity only relative to the 100 MHz ScalePipe design;
  * it does not incorporate the separate generated-RTL weight-address workaround.
  */
object Zu19egGemmini32x32Configs {
  val inferenceConfig = Zu19egGemminiConfigs.inferenceConfig.copy(
    tileRows = 1,
    tileColumns = 1,
    meshRows = 32,
    meshColumns = 32,
    sp_capacity = CapacityInKilobytes(512),
    acc_capacity = CapacityInKilobytes(128),
    headerFileName = "gemmini_params_zu19eg_32x32_sp512_acc128.h",
    acc_scale_fp32_pipeline = true,
    mvin_scale_fp32_pipeline = true,
    mvin_scale_args = Zu19egGemminiConfigs.inferenceConfig.mvin_scale_args.map(
      _.copy(latency = 8)),
    exact_resadd_pipeline = true)
}

class GemminiRocketZu19eg32x32Sp512Acc128100MHzScalePipeConfig extends Config(
  // DefaultGemminiConfig appends BuildRoCC. Do not stack the DIM=16 Gemmini config.
  new gemmini.DefaultGemminiConfig(Zu19egGemmini32x32Configs.inferenceConfig) ++
  new WithZu19egPMU(8) ++ new WithZu19egTweaks(100.0) ++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(nWays = 4, capacityKB = 128) ++
  new chipyard.config.WithSystemBusWidth(128) ++ new chipyard.RocketConfig)
