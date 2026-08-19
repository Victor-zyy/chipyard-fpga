package chipyard.fpga.ku5p

import sys.process._

import org.chipsalliance.cde.config.Config
import freechips.rocketchip.devices.tilelink.BootROMLocated
import freechips.rocketchip.resources.DTSTimebase
import freechips.rocketchip.subsystem.{ExtMem, SystemBusKey}
import freechips.rocketchip.util.SystemFileName
import sifive.blocks.devices.spi.{PeripherySPIKey, SPIParams}
import sifive.blocks.devices.uart.{PeripheryUARTKey, UARTParams}
import sifive.fpgashells.shell.xilinx.Ku5pDDRSize
import chipyard.daq._
import testchipip.serdes.SerialTLKey

class WithKu5pPeripherals extends Config((site, here, up) => {
  case PeripheryUARTKey => List(UARTParams(address = BigInt(0x64000000L)))
  case PeripherySPIKey => List(SPIParams(rAddress = BigInt(0x64001000L)))
  case PeripheryDAQKey => List(DAQParams(address = BigInt(0x64002000L), channels = 4, sampleWidth = 16, fifoDepth = 256))
})

class WithKu5pSystemModifications extends Config((site, here, up) => {
  case DTSTimebase => BigInt(1000000)
  case BootROMLocated(location) => up(BootROMLocated(location), site).map { params =>
    val freqMHz = site(SystemBusKey).dtsFrequency.get / 1000000
    val command = s"make -C fpga/src/main/resources/ku5p/sdboot PBUS_CLK=$freqMHz bin"
    require(command.! == 0, "Failed to build KU5P SD-card boot ROM")
    params.copy(
      hang = 0x10000,
      contentFileName = SystemFileName("./fpga/src/main/resources/ku5p/sdboot/build/sdboot.bin"))
  }
  case ExtMem => up(ExtMem, site).map { memory =>
    memory.copy(master = memory.master.copy(size = site(Ku5pDDRSize)))
  }
  case SerialTLKey => Nil
})

class WithKu5pFrequency(freqMHz: Double) extends Config(
  new chipyard.harness.WithHarnessBinderClockFreqMHz(freqMHz) ++
  new chipyard.config.WithSystemBusFrequency(freqMHz) ++
  new chipyard.config.WithPeripheryBusFrequency(freqMHz) ++
  new chipyard.config.WithControlBusFrequency(freqMHz) ++
  new chipyard.config.WithFrontBusFrequency(freqMHz) ++
  new chipyard.config.WithMemoryBusFrequency(freqMHz))

/**
  * Add Rocket HPM counters and describe their event selectors to OpenSBI.
  * Rocket implements mhpmcounter3 through mhpmcounter(2 + nPerfCounters),
  * so bits 3 and above are set in the OpenSBI counter bitmap.
  */
class WithKu5pPMU(nPerfCounters: Int = 8) extends Config(
  new Config((site, here, up) => {
    case chipyard.PMUDeviceTreeKey =>
      require(nPerfCounters >= 1 && nPerfCounters <= 29,
        s"KU5P PMU supports 1 to 29 programmable counters, got $nPerfCounters")

      val counterMask = ((BigInt(1) << nPerfCounters) - 1) << 3
      Some(chipyard.PMUDeviceTreeParams(
        eventToMhpmevent = Seq(
          // Generic SBI hardware events. Cache references is Rocket's
          // closest approximation: cycles in which I$ or D$ is blocked.
          chipyard.PMUEventToMhpmevent(eventIdx = BigInt(0x00003), selector = BigInt(0x1801)),
          chipyard.PMUEventToMhpmevent(eventIdx = BigInt(0x00004), selector = BigInt(0x0302)),
          chipyard.PMUEventToMhpmevent(eventIdx = BigInt(0x00005), selector = BigInt(0x4000)),
          chipyard.PMUEventToMhpmevent(eventIdx = BigInt(0x00006), selector = BigInt(0x6001))),
        eventToMhpmcounters = Seq(
          chipyard.PMUEventToMhpmcounters(
            eventIdxStart = BigInt(0x00003),
            eventIdxEnd = BigInt(0x00006),
            counterMask = counterMask)),
        rawEventToMhpmcounters = Seq(
          // Rocket selector bits [7:0] choose an event set. Bits [31:8]
          // are the event mask: 18 retired, 11 stall, and 6 memory events.
          chipyard.PMURawEventToMhpmcounters(
            selector = BigInt(0x0),
            selectorMask = BigInt("fffffffffc0000ff", 16),
            counterMask = counterMask),
          chipyard.PMURawEventToMhpmcounters(
            selector = BigInt(0x1),
            selectorMask = BigInt("fffffffffff800ff", 16),
            counterMask = counterMask),
          chipyard.PMURawEventToMhpmcounters(
            selector = BigInt(0x2),
            selectorMask = BigInt("ffffffffffffc0ff", 16),
            counterMask = counterMask))))
  }) ++
  new chipyard.config.WithNPerfCounters(nPerfCounters))

class WithKu5pTweaks(freqMHz: Double = 50.0) extends Config(
  new chipyard.iobinders.WithSPIIOPunchthrough(12.5) ++
  new chipyard.harness.WithAllClocksFromHarnessClockInstantiator ++
  new chipyard.clocking.WithPassthroughClockGenerator ++
  new chipyard.config.WithUniformBusFrequencies(freqMHz) ++
  new WithKu5pFrequency(freqMHz) ++
  new WithKu5pUART ++
  new WithKu5pSPISDCard ++
  new WithKu5pDDRMem ++
  new WithKu5pPeripherals ++
  new chipyard.config.WithTLBackingMemory ++
  new WithKu5pSystemModifications ++
  new chipyard.config.WithNoDebug ++
  new freechips.rocketchip.subsystem.WithoutTLMonitors ++
  new freechips.rocketchip.subsystem.WithNMemoryChannels(1))

class RocketKu5pConfig extends Config(
  new WithKu5pTweaks ++
  new WithKu5pPMU(8) ++
  // A modest single-bank inclusive L2 for learning and FPGA-friendly resource use.
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(
    nWays = 4,
    capacityKB = 128) ++
  new chipyard.RocketConfig)

/*
 *  For - simulators RocketKu5pSimConfig
 *
 **/
class WithKu5pSimPeripherals extends Config((site, here, up) => {
  case PeripheryUARTKey => List(UARTParams(address = BigInt(0x64000000L)))
  case PeripherySPIKey => Nil
  case PeripheryDAQKey => List(DAQParams(address = BigInt(0x64002000L)))
})

class RocketKu5pSimConfig extends Config(
  new WithKu5pPMU(8) ++
  new WithKu5pSimPeripherals++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(
    nWays = 4,
    capacityKB = 128) ++
  new chipyard.RocketConfig)


object Ku5pGemminiConfigs {
  val inferenceConfig = gemmini.GemminiConfigs.defaultConfig.copy(
    dataflow = gemmini.Dataflow.WS,
    has_training_convs = false,
    use_dsp_for_mac = true)
}

class GemminiRocketKu5pConfig extends Config(
  new gemmini.DefaultGemminiConfig(Ku5pGemminiConfigs.inferenceConfig) ++
  new WithKu5pPMU(8) ++
  new WithKu5pTweaks ++  
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(
    nWays = 4,
    capacityKB = 128) ++
  //new chipyard.config.WithBroadcastManager ++
  new chipyard.config.WithSystemBusWidth(128) ++
  new chipyard.RocketConfig)
