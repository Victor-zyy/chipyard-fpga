package chipyard.fpga.zu19eg

import org.chipsalliance.cde.config.Config

/** Full Rocket/Gemmini RTL simulation using the ZU19EG 32x32 accelerator.
  *
  * Reuse every accelerator parameter, including the ScalePipe, SiLU LUT,
  * exact ResAdd/Gather, SRAM geometry and DMA settings, from the FPGA config.
  * Only the generated software header name differs, to protect the board ABI.
  * RocketZu19egSimConfig supplies the standard Chipyard TestHarness boot,
  * simulated memory, UART at 0x64000000, PMU and single-bank 128 KiB L2.
  * Do not inherit WithZu19egTweaks: its DDR/SD/UART binders require Zu19egHarness.
  * No accelerator datapath changes or fault-injection settings are enabled here.
  */
class GemminiRocketZu19eg32x32Sp512Acc128100MHzScalePipeSimConfig extends Config(
  // The base Rocket simulation config has no Gemmini; append exactly one.
  new gemmini.DefaultGemminiConfig(
    Zu19egGemmini32x32Configs.inferenceConfig.copy(
      headerFileName = "gemmini_params_zu19eg_32x32_sp512_acc128_sim.h")) ++
  new chipyard.config.WithUniformBusFrequencies(100.0) ++
  new WithZu19egFrequency(100.0) ++
  new chipyard.config.WithSystemBusWidth(128) ++
  new RocketZu19egSimConfig)
