package chipyard.fpga.zu19eg

import sys.process._

import org.chipsalliance.cde.config.Config
import freechips.rocketchip.devices.tilelink.BootROMLocated
import freechips.rocketchip.resources.DTSTimebase
import freechips.rocketchip.subsystem.{ExtMem, SystemBusKey}
import freechips.rocketchip.util.SystemFileName
import sifive.blocks.devices.spi.{PeripherySPIKey, SPIParams}
import sifive.blocks.devices.uart.{PeripheryUARTKey, UARTParams}
import sifive.fpgashells.shell.xilinx.Zu19egDDRSize
import testchipip.serdes.SerialTLKey

class WithZu19egPeripherals extends Config((site, here, up) => {
  case PeripheryUARTKey => List(UARTParams(address = BigInt(0x64000000L)))
  case PeripherySPIKey => List(SPIParams(rAddress = BigInt(0x64001000L)))
})

class WithZu19egSystemModifications extends Config((site, here, up) => {
  case DTSTimebase => BigInt(1000000)
  case BootROMLocated(location) => up(BootROMLocated(location), site).map { params =>
    val freqMHz = site(SystemBusKey).dtsFrequency.get / 1000000
    // Reuse the proven SPI boot sources, but never overwrite KU5P build outputs.
    val source = "fpga/src/main/resources/ku5p/sdboot"
    val build = new java.io.File(s"fpga/src/main/resources/zu19eg/sdboot/build-${freqMHz}MHz").getAbsolutePath
    val command = s"make -B -C $source BUILD_DIR=$build PBUS_CLK=$freqMHz bin"
    require(command.! == 0, "Failed to build ZU19EG SD-card boot ROM")
    params.copy(
      hang = 0x10000,
      contentFileName = SystemFileName(s"$build/sdboot.bin"))
  }
  case ExtMem => up(ExtMem, site).map { memory =>
    memory.copy(master = memory.master.copy(size = site(Zu19egDDRSize)))
  }
  case SerialTLKey => Nil
})

class WithZu19egFrequency(freqMHz: Double) extends Config(
  new chipyard.harness.WithHarnessBinderClockFreqMHz(freqMHz) ++
  new chipyard.config.WithSystemBusFrequency(freqMHz) ++
  new chipyard.config.WithPeripheryBusFrequency(freqMHz) ++
  new chipyard.config.WithControlBusFrequency(freqMHz) ++
  new chipyard.config.WithFrontBusFrequency(freqMHz) ++
  new chipyard.config.WithMemoryBusFrequency(freqMHz))

// Same Rocket PMU event encodings as KU5P; no board-specific pins involved.
class WithZu19egPMU(nPerfCounters: Int = 8)
    extends chipyard.fpga.ku5p.WithKu5pPMU(nPerfCounters)

class WithZu19egTweaks(freqMHz: Double = 50.0) extends Config(
  new chipyard.iobinders.WithSPIIOPunchthrough(12.5) ++
  new chipyard.harness.WithAllClocksFromHarnessClockInstantiator ++
  new chipyard.clocking.WithPassthroughClockGenerator ++
  new chipyard.config.WithUniformBusFrequencies(freqMHz) ++
  new WithZu19egFrequency(freqMHz) ++
  new WithZu19egUART ++
  new WithZu19egSPISDCard ++
  new WithZu19egDDRMem ++
  new WithZu19egPeripherals ++
  new chipyard.config.WithTLBackingMemory ++
  new WithZu19egSystemModifications ++
  new chipyard.config.WithNoDebug ++
  new freechips.rocketchip.subsystem.WithoutTLMonitors ++
  new freechips.rocketchip.subsystem.WithNMemoryChannels(1))

class RocketZu19egConfig extends Config(
  new WithZu19egTweaks ++
  new WithZu19egPMU(8) ++
  // A modest single-bank inclusive L2 for learning and FPGA-friendly resource use.
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(
    nWays = 4,
    capacityKB = 128) ++
  new chipyard.RocketConfig)

/*
 *  For - simulators RocketZu19egSimConfig
 *
 **/
class WithZu19egSimPeripherals extends Config((site, here, up) => {
  case PeripheryUARTKey => List(UARTParams(address = BigInt(0x64000000L)))
  case PeripherySPIKey => Nil
})

class RocketZu19egSimConfig extends Config(
  new freechips.rocketchip.subsystem.WithExtMemSize(BigInt("100000000", 16)) ++
  new WithZu19egPMU(8) ++
  new WithZu19egSimPeripherals++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(
    nWays = 4,
    capacityKB = 128) ++
  new chipyard.RocketConfig)


/** Keep the validated KU5P inference ISA and default SRAM capacities.
  * No ACC128, VPU, PE expansion, DAQ or PS instance in this port.
  * Use an independent generated header to protect the KU5P software ABI.
  */
object Zu19egGemminiConfigs {
  val inferenceConfig = chipyard.fpga.ku5p.Ku5pGemminiConfigs.inferenceConfig.copy(
    headerFileName = "gemmini_params_zu19eg.h")
}

class GemminiRocketZu19egConfig extends Config(
  new gemmini.DefaultGemminiConfig(Zu19egGemminiConfigs.inferenceConfig) ++
  new WithZu19egPMU(8) ++ new WithZu19egTweaks ++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(nWays = 4, capacityKB = 128) ++
  new chipyard.config.WithSystemBusWidth(128) ++ new chipyard.RocketConfig)

/** Frequency-only A/B variants. Preserve the 50 MHz configs above.
  * The harness PLL follows SystemBusKey; DDR reference/UI clocks stay 300 MHz.
  * Periphery frequency also updates UART reset divisors and the DTS timebase.
  * SD boot is rebuilt into build-100MHz; its SPI divisors follow PBUS_CLK.
  * Gemmini geometry/ISA and the generated software header are unchanged.
  */
class RocketZu19eg100MHzConfig extends Config(
  new chipyard.config.WithUniformBusFrequencies(100.0) ++
  new WithZu19egFrequency(100.0) ++
  new RocketZu19egConfig)

class GemminiRocketZu19eg100MHzConfig extends Config(
  new chipyard.config.WithUniformBusFrequencies(100.0) ++
  new WithZu19egFrequency(100.0) ++
  new GemminiRocketZu19egConfig)

// Timing experiment: same ISA, array, memory sizes and eight-cycle scale latency.
// The original 50/100 MHz configurations retain the legacy scaler for A/B.
class GemminiRocketZu19eg100MHzScalePipeConfig extends Config(
  new gemmini.DefaultGemminiConfig(Zu19egGemminiConfigs.inferenceConfig.copy(
    acc_scale_fp32_pipeline = true)) ++
  // DefaultGemminiConfig appends BuildRoCC; do not stack it on a config that
  // already contains Gemmini, or both accelerators would decode custom3.
  new WithZu19egPMU(8) ++ new WithZu19egTweaks(100.0) ++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(nWays = 4, capacityKB = 128) ++
  new chipyard.config.WithSystemBusWidth(128) ++ new chipyard.RocketConfig)

class GemminiRocketZu19egSimConfig extends Config(
  new gemmini.DefaultGemminiConfig(Zu19egGemminiConfigs.inferenceConfig) ++
  new WithZu19egPMU(8) ++ new WithZu19egSimPeripherals ++
  new freechips.rocketchip.subsystem.WithExtMemSize(BigInt("100000000", 16)) ++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(nWays = 4, capacityKB = 128) ++
  new chipyard.config.WithSystemBusWidth(128) ++ new chipyard.RocketConfig)
