package chipyard.fpga.zu19eg

import org.chipsalliance.cde.config.Config
import gemmini.{GemminiOrderingDiagnosticKey, OrderingDiagnosticParams}

/** Match ALL scaler switches of the 100 MHz board. TestHarness only, no PS/MIG.
  * A separate generated header protects the existing board header.
  */
class GemminiRocketZu19egScalePipeSimConfig extends Config(
  new gemmini.DefaultGemminiConfig(Zu19egGemminiConfigs.inferenceConfig.copy(
    headerFileName = "gemmini_params_zu19eg_ordering.h",
    acc_scale_fp32_pipeline = true,
    mvin_scale_fp32_pipeline = true,
    mvin_scale_args = Zu19egGemminiConfigs.inferenceConfig.mvin_scale_args.map(_.copy(latency = 8)),
    exact_resadd_pipeline = true)) ++
  new WithZu19egPMU(8) ++ new WithZu19egSimPeripherals ++
  new WithZu19egFrequency(100.0) ++
  new freechips.rocketchip.subsystem.WithExtMemSize(BigInt("100000000", 16)) ++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(nWays = 4, capacityKB = 128) ++
  new chipyard.config.WithSystemBusWidth(128) ++ new chipyard.RocketConfig)

class WithGemminiOrderingDiagnostic(delayCycles: Int) extends Config((site, here, up) => {
  case GemminiOrderingDiagnosticKey => OrderingDiagnosticParams(trace = true, readDelayCycles = delayCycles)
})

// Natural memory response order; observation only.
class GemminiRocketZu19egOrderingSimConfig extends Config(
  new WithGemminiOrderingDiagnostic(0) ++ new GemminiRocketZu19egScalePipeSimConfig)

// Same datapath; force allowed inter-source reordering. NEVER build for FPGA.
class GemminiRocketZu19egOrderingSkewSimConfig extends Config(
  new WithGemminiOrderingDiagnostic(4096) ++ new GemminiRocketZu19egScalePipeSimConfig)
