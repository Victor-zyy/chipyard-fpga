package chipyard.fpga.ku5p

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

class Ku5pHarness(override implicit val p: Parameters) extends Ku5pShellBasicOverlays {
  def dp = designParameters

  require(dp(ClockInputOverlayKey).size == 2, "KU5P requires the 50 MHz system and 100 MHz DDR clocks")
  val placedClocks = dp(ClockInputOverlayKey).map(_.place(ClockInputDesignInput()))
  val sysClockNode = placedClocks.head.overlayOutput.node

  val harnessSysPLL = dp(PLLFactoryKey)()
  harnessSysPLL := sysClockNode

  val dutFreqMHz = (dp(SystemBusKey).dtsFrequency.get / 1000000).toInt
  val dutClock = ClockSinkNode(freqMHz = dutFreqMHz)
  val dutWrangler = LazyModule(new ResetWrangler)
  val dutGroup = ClockGroup()
  dutClock := dutWrangler.node := dutGroup := harnessSysPLL
  println(s"KU5P FPGA system clock: $dutFreqMHz MHz")

  val ioUART = BundleBridgeSource(() => new UARTPortIO(dp(PeripheryUARTKey).head))
  dp(UARTOverlayKey).head.place(UARTDesignInput(ioUART))

  val ioSPI = BundleBridgeSource(() => new SPIPortIO(dp(PeripherySPIKey).head))
  dp(SPIOverlayKey).head.place(SPIDesignInput(dp(PeripherySPIKey).head, ioSPI))

  val ddrOverlay = dp(DDROverlayKey).head
    .place(DDRDesignInput(dp(ExtTLMem).get.master.base, dutWrangler.node, harnessSysPLL))
    .asInstanceOf[DDRKu5pPlacedOverlay]

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

  override lazy val module = new Ku5pHarnessImp(this)
}

class Ku5pHarnessImp(val ku5pOuter: Ku5pHarness)
    extends LazyRawModuleImp(ku5pOuter)
    with HasHarnessInstantiators {
  override def provideImplicitClockToLazyChildren = true

  // Keep the board fan at full speed once the FPGA configuration is active.
  // Q9 inverts FAN_PWM: low turns Q9 off and lets R250 pull the fan PWM input high.
  val fanPWM = IO(Output(Bool())).suggestName("fan_pwm")
  ku5pOuter.xdc.addPackagePin(IOPin(fanPWM), "E15")
  ku5pOuter.xdc.addIOStandard(IOPin(fanPWM), "LVCMOS18")
  fanPWM := false.B

  val sysReset = IO(Input(Bool())).suggestName("sys_rst")
  ku5pOuter.xdc.addPackagePin(IOPin(sysReset), "P19")
  ku5pOuter.xdc.addIOStandard(IOPin(sysReset), "LVCMOS12")

  val resetIBUF = Module(new IBUF)
  resetIBUF.io.I := sysReset

  val sysClock = ku5pOuter.sysClockNode.out.head._1.clock
  val powerOnReset = PowerOnResetFPGAOnly(sysClock)
  ku5pOuter.sdc.addAsyncPath(Seq(powerOnReset))
  ku5pOuter.pllReset := resetIBUF.io.O || powerOnReset

  val referenceClockFreqMHz = ku5pOuter.dutFreqMHz
  val referenceClock = ku5pOuter.dutClock.in.head._1.clock
  val referenceReset = ku5pOuter.dutClock.in.head._1.reset
  def success = { require(false, "Unused"); false.B }

  childClock := harnessBinderClock
  childReset := harnessBinderReset

  ku5pOuter.ddrOverlay.mig.module.clock := harnessBinderClock
  ku5pOuter.ddrOverlay.mig.module.reset := harnessBinderReset
  ku5pOuter.ddrBlockDuringReset.module.clock := harnessBinderClock
  ku5pOuter.ddrBlockDuringReset.module.reset :=
    harnessBinderReset.asBool || !ku5pOuter.ddrOverlay.calibComplete

  // The board LEDs use high-side inputs into low-side NMOS drivers: high means on.
  ku5pOuter.leds.foreach(_ := false.B)
  if (ku5pOuter.leds.nonEmpty) {
    withClockAndReset(referenceClock, referenceReset) {
      val counterWidth = log2Ceil((referenceClockFreqMHz * 1000000).toInt)
      val counter = RegInit(0.U(counterWidth.W))
      counter := counter + 1.U
      ku5pOuter.leds.head := counter(counter.getWidth - 1)
    }
  }
  if (ku5pOuter.leds.size >= 8) {
    ku5pOuter.leds(7) := ku5pOuter.ddrOverlay.calibComplete
  }

  instantiateChipTops()
}
