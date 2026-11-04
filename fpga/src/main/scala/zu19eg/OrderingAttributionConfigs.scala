package chipyard.fpga.zu19eg

import org.chipsalliance.cde.config.Config

/** Configuration ablation, NOT a checkout of an unmodified upstream Gemmini.
  * The legacy scaler branches predate b8c3b48c8c20a9f13819e894a2958416faa89350.
  * Keep the busy fix, memory system, clocks, geometry and ordering injector.
  * Never use these diagnostic configurations for FPGA bitstreams.
  */
object Zu19egOrderingAttribution {
  private val base = Zu19egGemminiConfigs.inferenceConfig
  require(base.meshRows * base.tileRows == 16 && base.meshColumns * base.tileColumns == 16)
  require(base.acc_banks * base.acc_bank_entries == 1024)
  require(!base.has_layer_matmul)

  val legacy = base.copy(
    headerFileName = "gemmini_params_zu19eg_ordering_legacy.h",
    acc_scale_fp32_pipeline = false,
    mvin_scale_fp32_pipeline = false,
    exact_resadd_pipeline = false,
    mvin_scale_args = base.mvin_scale_args.map(_.copy(latency = 4)))

  // Removes custom frontends and LUT/Exact arithmetic. Shared source files are
  // still the current revision; a failure here is NOT proof of an upstream bug.
  val noExt = legacy.copy(
    headerFileName = "gemmini_params_zu19eg_ordering_noext.h",
    has_silu_lut = false,
    has_exact_resadd = false,
    has_exact_gather = false,
    has_layer_matmul = false,
    use_dsp_for_mac = false)
}

// Match GemminiRocketZu19egScalePipeSimConfig's SoC without stacking BuildRoCC.
class Zu19egOrderingAttributionSoCConfig extends Config(
  new WithZu19egPMU(8) ++ new WithZu19egSimPeripherals ++
  new WithZu19egFrequency(100.0) ++
  new freechips.rocketchip.subsystem.WithExtMemSize(BigInt("100000000", 16)) ++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(nWays = 4, capacityKB = 128) ++
  new chipyard.config.WithSystemBusWidth(128) ++ new chipyard.RocketConfig)

class GemminiRocketZu19egOrderingLegacySimConfig extends Config(
  new WithGemminiOrderingDiagnostic(0) ++
  new gemmini.DefaultGemminiConfig(Zu19egOrderingAttribution.legacy) ++
  new Zu19egOrderingAttributionSoCConfig)

class GemminiRocketZu19egOrderingLegacySkewSimConfig extends Config(
  new WithGemminiOrderingDiagnostic(4096) ++
  new gemmini.DefaultGemminiConfig(Zu19egOrderingAttribution.legacy) ++
  new Zu19egOrderingAttributionSoCConfig)

class GemminiRocketZu19egOrderingNoExtSimConfig extends Config(
  new WithGemminiOrderingDiagnostic(0) ++
  new gemmini.DefaultGemminiConfig(Zu19egOrderingAttribution.noExt) ++
  new Zu19egOrderingAttributionSoCConfig)

class GemminiRocketZu19egOrderingNoExtSkewSimConfig extends Config(
  new WithGemminiOrderingDiagnostic(4096) ++
  new gemmini.DefaultGemminiConfig(Zu19egOrderingAttribution.noExt) ++
  new Zu19egOrderingAttributionSoCConfig)
