// See LICENSE for license details.
package chipyard.fpga.microphasea7

import chisel3._
import chisel3.util._
import freechips.rocketchip.diplomacy._
import org.chipsalliance.cde.config.{Parameters}
import freechips.rocketchip.tilelink._
import freechips.rocketchip.subsystem.{SystemBusKey}
import freechips.rocketchip.prci._
import sifive.fpgashells.shell.xilinx._
import sifive.fpgashells.shell._
import sifive.fpgashells.clocks._

import sifive.blocks.devices.uart._
import sifive.blocks.devices.spi._

import chipyard._
import chipyard.harness._

class MicrophaseA7Harness(override implicit val p: Parameters) extends MicrophaseA7Shell {
  def dp = designParameters

  val clockOverlay = dp(ClockInputOverlayKey).map(_.place(ClockInputDesignInput())).head
  val harnessSysPLL = dp(PLLFactoryKey)
  val harnessSysPLLNode = harnessSysPLL()
  val dutFreqMHz = (dp(SystemBusKey).dtsFrequency.get / (1000 * 1000)).toInt
  val dutClock = ClockSinkNode(freqMHz = dutFreqMHz)
  println(s"MicrophaseA7 FPGA Base Clock Freq: ${dutFreqMHz} MHz")
  val dutWrangler = LazyModule(new ResetWrangler())
  val dutGroup = ClockGroup()
  dutClock := dutWrangler.node := dutGroup := harnessSysPLLNode

  harnessSysPLLNode := clockOverlay.overlayOutput.node

  val io_uart_bb = BundleBridgeSource(() => new UARTPortIO(dp(PeripheryUARTKey).headOption.getOrElse(UARTParams(0))))
  val uartOverlay = dp(UARTOverlayKey).head.place(UARTDesignInput(io_uart_bb))
  val io_spi_bb = dp(PeripherySPIKey).headOption.map { spiParams =>
    BundleBridgeSource(() => new SPIPortIO(spiParams))
  }
  
  val sdcardOverlay = dp(PeripherySPIKey).headOption.map { spiParams =>
    dp(SPIOverlayKey).head.place(SPIDesignInput(spiParams, io_spi_bb.get))
  }

  // Optional DDR
  val ddrOverlay = if (p(MicrophaseA7ShellDDR)) Some(dp(DDROverlayKey).head.place(DDRDesignInput(dp(ExtTLMem).get.master.base, dutWrangler.node, harnessSysPLLNode)).asInstanceOf[DDRMicrophaseA7PlacedOverlay]) else None
  val ddrClient = if (p(MicrophaseA7ShellDDR)) Some(TLClientNode(Seq(TLMasterPortParameters.v1(Seq(TLMasterParameters.v1(
    name = "chip_ddr",
    sourceId = IdRange(0, 1 << dp(ExtTLMem).get.master.idBits)
  )))))) else None
  val ddrBlockDuringReset = if (p(MicrophaseA7ShellDDR)) Some(LazyModule(new TLBlockDuringReset(4))) else None
  if (p(MicrophaseA7ShellDDR)) { ddrOverlay.get.overlayOutput.ddr := ddrBlockDuringReset.get.node := ddrClient.get }

  val ledOverlays = dp(LEDOverlayKey).map(_.place(LEDDesignInput()))
  val all_leds = ledOverlays.map(_.overlayOutput.led)

  override lazy val module = new HarnessLikeImpl

  class HarnessLikeImpl extends Impl with HasHarnessInstantiators {
    all_leds.foreach(_ := DontCare)
    clockOverlay.overlayOutput.node.out(0)._1.reset := ~resetPin

    val clk_50mhz = clockOverlay.overlayOutput.node.out.head._1.clock

    // A7-LITE only has two user LEDs. Do not keep NexysVideo's other_leds indexing.
    if (all_leds.nonEmpty) {
      withClockAndReset(clk_50mhz, dutClock.in.head._1.reset) {
        val counter = RegInit(0.U(26.W))
        counter := counter + 1.U
        all_leds(0) := counter(25)
      }
    }

    if (p(MicrophaseA7ShellDDR) && all_leds.size >= 2) {
      val migPort = ddrOverlay.get.mig.module.io.port
      //all_leds(1) := ~ddrOverlay.get.mig.module.io.port.init_calib_complete
      // LED1 亮：init_calib_complete = 1，DDR 校准完成
      // LED1 灭：init_calib_complete = 0，DDR 校准未完成
      all_leds(1) := !migPort.init_calib_complete
    }

    harnessSysPLL.plls.foreach(_._1.getReset.get := pllReset)

    def referenceClockFreqMHz = dutFreqMHz
    def referenceClock = dutClock.in.head._1.clock
    def referenceReset = dutClock.in.head._1.reset
    def success = { require(false, "Unused"); false.B }

    if (p(MicrophaseA7ShellDDR)) { 
      ddrOverlay.get.mig.module.clock := harnessBinderClock
      ddrOverlay.get.mig.module.reset := harnessBinderReset
      ddrBlockDuringReset.get.module.clock := harnessBinderClock
      ddrBlockDuringReset.get.module.reset := harnessBinderReset.asBool || !ddrOverlay.get.mig.module.io.port.init_calib_complete
    }

    instantiateChipTops()
  }
}
