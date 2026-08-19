// See LICENSE for license details.
package chipyard.fpga.microphasea7

import sys.process._

import org.chipsalliance.cde.config._
import freechips.rocketchip.subsystem._
import freechips.rocketchip.devices.debug._
import freechips.rocketchip.devices.tilelink._
import org.chipsalliance.diplomacy.lazymodule._
import freechips.rocketchip.system._
import freechips.rocketchip.tile._
import freechips.rocketchip.util.{SystemFileName}
import freechips.rocketchip.resources.{DTSTimebase}

import gemmini._

import sifive.blocks.devices.uart._
import sifive.blocks.devices.spi._
import sifive.fpgashells.shell.{DesignKey}

import testchipip.serdes.{SerialTLKey}

import chipyard.{BuildSystem}

// don't use FPGAShell's DesignKey
class WithNoDesignKey extends Config((site, here, up) => {
  case DesignKey => (p: Parameters) => new SimpleLazyRawModule()(p)
})

// Small Gemmini configuration optimized for Artix-7 FPGA resources.
// Keep the array usable for integer matmul, but avoid the default hardfloat
// scale/activation units which create very long FPGA timing paths.
class WithSmallGemmini extends Config((site, here, up) => {
  case BuildRoCC => up(BuildRoCC) ++ Seq(
    (p: Parameters) => {
      implicit val q = p
      val gemminiConfig: GemminiArrayConfig[_root_.chisel3.SInt, _root_.gemmini.Float, _root_.gemmini.Float] =
        GemminiConfigs.defaultConfig.copy(
        // Reduce systolic array size from 16x16 to 8x8
        meshRows = 8,
        meshColumns = 8,

        // Reduce memory capacity to fit Artix-7 BRAM
        sp_capacity = CapacityInKilobytes(64),
        acc_capacity = CapacityInKilobytes(32),

        // Disable unused features to save resources
        has_training_convs = false,
        has_max_pool = false,
        has_loop_conv = true,
        has_dw_convs = false,
        has_first_layer_optimizations = false,
        has_nonlinear_activations = false,

        // Use weight-stationary dataflow for inference
        dataflow = Dataflow.WS,

        // Reduce queue depths to save resources
        ld_queue_length = 8,
        st_queue_length = 4,
        ex_queue_length = 8,

        // Allow software to read full int32 accumulator results and apply
        // quantization scales on the CPU, avoiding Gemmini's hardfloat scaler.
        acc_read_full_width = true,
        ex_read_from_acc = false,
        ex_write_to_spad = false,
        hardcode_d_to_garbage_addr = true,

        // The default float scale units put hardfloat conversion/multiply/saturate
        // logic before Gemmini's Pipe registers. On Artix-7 this was the observed
        // WNS=-20ns path, so the FPGA config uses identity scaling in hardware.
        mvin_scale_args = Option.empty[ScaleArguments[_root_.chisel3.SInt, _root_.gemmini.Float]],
        mvin_scale_acc_args = Option.empty[ScaleArguments[_root_.chisel3.SInt, _root_.gemmini.Float]],
        acc_scale_args = Option.empty[ScaleArguments[_root_.chisel3.SInt, _root_.gemmini.Float]],

        // Add a little internal array pipelining for FPGA place-and-route.
        tile_latency = 1,
        mesh_output_delay = 2,
        num_counter = 8
      )
      val gemmini = LazyModule(new Gemmini(gemminiConfig))
      gemmini
    }
  )
})

// DOC include start: WithMicrophaseA7Tweaks and Rocket
class WithMicrophaseA7Tweaks(freqMHz: Double = 50) extends Config(
  new WithMicrophaseA7UARTTSI ++
  new WithMicrophaseA7DDRTL ++
  new WithNoDesignKey ++
  new testchipip.tsi.WithUARTTSIClient(BigInt(576000))++
  new chipyard.harness.WithSerialTLTiedOff ++
  new chipyard.harness.WithHarnessBinderClockFreqMHz(freqMHz) ++
  new chipyard.config.WithUniformBusFrequencies(freqMHz) ++
  new chipyard.harness.WithAllClocksFromHarnessClockInstantiator ++
  new chipyard.clocking.WithPassthroughClockGenerator ++
  new chipyard.config.WithNoDebug ++ // no jtag
  new chipyard.config.WithNoUART ++ // use UART for the UART-TSI thing instad
  new chipyard.config.WithTLBackingMemory ++ // FPGA-shells converts the AXI to TL for us
  new freechips.rocketchip.subsystem.WithExtMemSize(BigInt(512) << 20) ++ // TODO: replace with Microphase A7 DDR capacity
  new freechips.rocketchip.subsystem.WithoutTLMonitors)

// DOC include end: WithMicrophaseA7Tweaks and Rocket

// DOC include start: WithTinyMicrophaseA7Tweaks and Rocket
class WithTinyMicrophaseA7Tweaks extends Config(
  new WithMicrophaseA7UARTTSI ++
  new WithNoDesignKey ++
  new sifive.fpgashells.shell.xilinx.WithNoMicrophaseA7ShellDDR ++ // no DDR
  new testchipip.tsi.WithUARTTSIClient(initBaudRate = BigInt(576000)) ++
  new chipyard.harness.WithSerialTLTiedOff ++
  new chipyard.harness.WithHarnessBinderClockFreqMHz(50) ++
  new chipyard.config.WithMemoryBusFrequency(50.0) ++
  new chipyard.config.WithFrontBusFrequency(50.0) ++
  new chipyard.config.WithSystemBusFrequency(50.0) ++
  new chipyard.config.WithPeripheryBusFrequency(50.0) ++
  new chipyard.config.WithControlBusFrequency(50.0) ++
  new chipyard.harness.WithAllClocksFromHarnessClockInstantiator ++
  new chipyard.clocking.WithPassthroughClockGenerator ++
  new chipyard.config.WithNoDebug ++ // no jtag
  new chipyard.config.WithNoUART ++ // use UART for the UART-TSI thing instad
  new freechips.rocketchip.subsystem.WithoutTLMonitors)




// Base Config for MicrophaseA7 and Verified OK
class RocketMicrophaseA7Config extends Config(
  new WithMicrophaseA7Tweaks ++
  new chipyard.config.WithBroadcastManager ++ // no l2
  new chipyard.RocketConfig)

class TinyRocketMicrophaseA7Config extends Config(
  new WithTinyMicrophaseA7Tweaks ++
  new chipyard.config.WithBroadcastManager ++ // no l2
  new chipyard.TinyRocketConfig)
  // DOC include end: WithTinyMicrophaseA7Tweaks and Rocket

class BringupMicrophaseA7Config extends Config(
  new WithMicrophaseA7Tweaks(freqMHz = 75) ++
  new chipyard.ChipBringupHostConfig)

// Gemmini + Rocket on Nexys Video with fast UART
class GemminiMicrophaseA7Config extends Config(
  new WithSmallGemmini ++
  new WithMicrophaseA7Tweaks ++
  new chipyard.config.WithBroadcastManager ++ // no L2 cache
  new chipyard.config.WithSystemBusWidth(128) ++ // Required for Gemmini
  new chipyard.RocketConfig)

// ============================================================
// Gemmini + SD Card Configuration
// ============================================================

class WithSystemModifications extends Config((site, here, up) => {
  case DTSTimebase => BigInt((1e6).toLong)
  case BootROMLocated(x) => up(BootROMLocated(x), site).map { p =>
    // invoke makefile for sdboot
    val freqMHz = (site(SystemBusKey).dtsFrequency.get / (1000 * 1000)).toLong
    val make = s"make -C fpga/src/main/resources/microphasea7/sdboot PBUS_CLK=${freqMHz} bin"
    require (make.! == 0, "Failed to build bootrom")
    p.copy(hang = 0x10000, contentFileName = SystemFileName(s"./fpga/src/main/resources/microphasea7/sdboot/build/sdboot.bin"))
  }
})

// SPI peripheral configuration (for SD card)
class WithMicrophaseA7SPI extends Config((site, here, up) => {
  case PeripherySPIKey => List(SPIParams(rAddress = BigInt(0x64001000L)))
})

class WithMicrophaseA7UART extends Config((site, here, up) => {
  case PeripheryUARTKey => List(UARTParams(address = BigInt(0x64000000L)))
})


// Tweaks with SD card support (based on FastUART version)

class WithMicrophaseA7TweaksSD(freqMHz: Double = 50) extends Config(
  new WithMicrophaseA7StandardUART ++
  new WithMicrophaseA7DDRTL ++
  new WithMicrophaseA7SDCard ++  // Connect onboard SD card
  new WithNoDesignKey ++
  new WithMicrophaseA7SPI ++                      // SPI controller
  new WithMicrophaseA7UART ++                      // UART controller
  new WithSystemModifications ++
  new chipyard.harness.WithSerialTLTiedOff ++
  new chipyard.harness.WithHarnessBinderClockFreqMHz(freqMHz) ++
  new chipyard.config.WithUniformBusFrequencies(freqMHz) ++
  new chipyard.harness.WithAllClocksFromHarnessClockInstantiator ++
  new chipyard.clocking.WithPassthroughClockGenerator ++
  new chipyard.config.WithNoDebug ++ // no jtag
  new chipyard.config.WithTLBackingMemory ++ // FPGA-shells converts the AXI to TL for us
  new freechips.rocketchip.subsystem.WithExtMemSize(BigInt(512) << 20) ++ // TODO: replace with Microphase A7 DDR capacity
  new freechips.rocketchip.subsystem.WithoutTLMonitors)

// Gemmini + SD card complete configuration
class GemminiMicrophaseA7SDConfig extends Config(
  new WithSmallGemmini ++                       // 8x8 Gemmini accelerator
  new WithMicrophaseA7TweaksSD++             // Tweaks with SD card
  new chipyard.config.WithBroadcastManager ++   // No L2 cache
  new chipyard.config.WithSystemBusWidth(128) ++ // Required for Gemmini
  new chipyard.RocketConfig)
