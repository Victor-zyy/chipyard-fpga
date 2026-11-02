package chipyard.fpga.zu19eg

import chisel3._
import chisel3.util.log2Ceil
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.prci._
import freechips.rocketchip.subsystem.SystemBusKey
import freechips.rocketchip.tilelink._
import org.chipsalliance.cde.config.Parameters
import sifive.blocks.devices.spi.{PeripherySPIKey, SPIPortIO}
import sifive.blocks.devices.uart.{PeripheryUARTKey, UARTPortIO}
import sifive.fpgashells.clocks._
import sifive.fpgashells.ip.xilinx.{IBUF, PowerOnResetFPGAOnly}
import sifive.fpgashells.shell._
import sifive.fpgashells.shell.xilinx._

import chipyard._
import chipyard.harness._

class Zu19egHarness(override implicit val p: Parameters) extends Zu19egShellBasicOverlays {
  def dp = designParameters

  require(dp(ClockInputOverlayKey).size == 1, "ZU19EG uses one shared 300 MHz PL differential reference")
  val placedClocks = dp(ClockInputOverlayKey).map(_.place(ClockInputDesignInput()))
  val sysClockNode = placedClocks.head.overlayOutput.node

  val harnessSysPLL = dp(PLLFactoryKey)()
  harnessSysPLL := sysClockNode

  val dutFreqMHz = (dp(SystemBusKey).dtsFrequency.get / 1000000).toInt
  val dutClock = ClockSinkNode(freqMHz = dutFreqMHz)
  val dutWrangler = LazyModule(new ResetWrangler)
  val dutGroup = ClockGroup()
  dutClock := dutWrangler.node := dutGroup := harnessSysPLL
  println(s"ZU19EG FPGA system clock: $dutFreqMHz MHz")

  val ioUART = BundleBridgeSource(() => new UARTPortIO(dp(PeripheryUARTKey).head))
  dp(UARTOverlayKey).head.place(UARTDesignInput(ioUART))

  val ioSPI = BundleBridgeSource(() => new SPIPortIO(dp(PeripherySPIKey).head))
  dp(SPIOverlayKey).head.place(SPIDesignInput(dp(PeripherySPIKey).head, ioSPI))

  val ddrOverlay = dp(DDROverlayKey).head
    .place(DDRDesignInput(dp(ExtTLMem).get.master.base, dutWrangler.node, harnessSysPLL))
    .asInstanceOf[DDRZu19egPlacedOverlay]

  val ddrClient = TLClientNode(Seq(TLMasterPortParameters.v1(Seq(TLMasterParameters.v1(
    name = "chip_ddr",
    sourceId = IdRange(0, 1 << dp(ExtTLMem).get.master.idBits)
  )))))
  val ddrBlockDuringReset = LazyModule(new TLBlockDuringReset(4))
  ddrOverlay.overlayOutput.ddr :=
    ddrBlockDuringReset.node :=
    TLWidthWidget(dp(ExtTLMem).get.master.beatBytes) :=
    ddrClient

  val leds = dp(LEDOverlayKey).map(_.place(LEDDesignInput()).overlayOutput.led)

  override lazy val module = new Zu19egHarnessImp(this)
}

class Zu19egHarnessImp(val zu19egOuter: Zu19egHarness)
    extends LazyRawModuleImp(zu19egOuter)
    with HasHarnessInstantiators {
  override def provideImplicitClockToLazyChildren = true

  val sysReset = IO(Input(Bool())).suggestName("sys_rst_n")
  zu19egOuter.xdc.addPackagePin(IOPin(sysReset), "AW17")
  zu19egOuter.xdc.addIOStandard(IOPin(sysReset), "LVCMOS12")

  val resetIBUF = Module(new IBUF)
  resetIBUF.io.I := sysReset

  val sysClock = zu19egOuter.sysClockNode.out.head._1.clock
  val powerOnReset = PowerOnResetFPGAOnly(sysClock)
  zu19egOuter.sdc.addAsyncPath(Seq(powerOnReset))
  zu19egOuter.pllReset := !resetIBUF.io.O || powerOnReset

  val referenceClockFreqMHz = zu19egOuter.dutFreqMHz
  val referenceClock = zu19egOuter.dutClock.in.head._1.clock
  val referenceReset = zu19egOuter.dutClock.in.head._1.reset
  def success = { require(false, "Unused"); false.B }

  childClock := harnessBinderClock
  childReset := harnessBinderReset

  zu19egOuter.ddrOverlay.mig.module.clock := harnessBinderClock
  zu19egOuter.ddrOverlay.mig.module.reset := harnessBinderReset
  zu19egOuter.ddrBlockDuringReset.module.clock := harnessBinderClock
  val memoryNotReady = freechips.rocketchip.util.ResetCatchAndSync(
    harnessBinderClock, harnessBinderReset.asBool || !zu19egOuter.ddrOverlay.calibComplete)
  zu19egOuter.ddrBlockDuringReset.module.reset := memoryNotReady

  // Six on-board LEDs, active LOW. AW17 is reset only, not also SW0.
  zu19egOuter.leds.foreach(_ := true.B)
  if (zu19egOuter.leds.nonEmpty) {
    withClockAndReset(referenceClock, referenceReset) {
      val counterWidth = log2Ceil((referenceClockFreqMHz * 1000000).toInt)
      val counter = RegInit(0.U(counterWidth.W))
      counter := counter + 1.U
      zu19egOuter.leds.head := !counter(counter.getWidth - 1)
    }
  }
  if (zu19egOuter.leds.size >= 6) {
    zu19egOuter.leds(5) := !zu19egOuter.ddrOverlay.calibComplete
  }

  instantiateChipTops()
}
