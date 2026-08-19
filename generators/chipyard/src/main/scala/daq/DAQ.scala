package chipyard.daq

import chisel3._
import chisel3.util._

import freechips.rocketchip.amba.axi4._
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.interrupts._
import freechips.rocketchip.prci._
import freechips.rocketchip.regmapper.RegField
import freechips.rocketchip.resources.{
  ResourceAddress => DTResourceAddress,
  ResourceBinding => DTResourceBinding,
  ResourcePermissions => DTResourcePermissions,
  SimpleDevice
}
import freechips.rocketchip.subsystem.{BaseSubsystem, PBUS}
import freechips.rocketchip.tilelink._

import org.chipsalliance.cde.config.{Field, Parameters}

case class DAQParams(
  address: BigInt,
  channels: Int = 4,
  sampleWidth: Int = 16,
  fifoDepth: Int = 256
) {
  require(channels == 4, "DAQ v1 requires 4 channels")
  require(sampleWidth == 16, "DAQ v1 requires 16-bit channels")
  require(fifoDepth > 0 && fifoDepth <= 0xffff)
  require((address & 0xfff) == 0, "DAQ MMIO base address must be 4 KiB aligned")
}

case object PeripheryDAQKey extends Field[Seq[DAQParams]](Nil)

class DAQCore(params: DAQParams) extends Module {
  val io = IO(new Bundle {
    val enable = Input(Bool())
    val samplePeriod = Input(UInt(32.W))
    val channelEnable = Input(UInt(4.W))
    val fifoWatermark = Input(UInt(16.W))
    val irqEnable = Input(UInt(2.W))
    val testPattern = Input(UInt(16.W))

    val fifoClear = Input(Bool())
    val counterClear = Input(Bool())
    val fifoPop = Input(Bool())
    val overflowClear = Input(Bool())

    val fifoLevel = Output(UInt(16.W))
    val dropCount = Output(UInt(32.W))
    val sampleCount = Output(UInt(32.W))
    val fifoData = Output(UInt(128.W))

    val fifoEmpty = Output(Bool())
    val fifoFull = Output(Bool())
    val overflow = Output(Bool())
    val irqStatus = Output(UInt(2.W))
    val irq = Output(Bool())
  })

  val timer = RegInit(0.U(32.W))
  val timestamp = RegInit(0.U(64.W))
  val sequence = RegInit(0.U(16.W))
  val lfsr = RegInit("hace1".U(16.W))
  val freeCounter = RegInit(0.U(16.W))
  val sampleCount = RegInit(0.U(32.W))
  val dropCount = RegInit(0.U(32.W))
  val overflow = RegInit(false.B)

  val effectivePeriod = Mux(io.samplePeriod === 0.U, 1.U, io.samplePeriod)
  val sampleEvent = io.enable && timer >= (effectivePeriod - 1.U)

  val feedback = lfsr(15) ^ lfsr(13) ^ lfsr(12) ^ lfsr(10)
  val lfsrNext = Cat(lfsr(14, 0), feedback)

  val ch0 = Mux(io.channelEnable(0), sequence, 0.U(16.W))
  val ch1 = Mux(io.channelEnable(1), lfsr, 0.U(16.W))
  val ch2 = Mux(io.channelEnable(2), freeCounter, 0.U(16.W))
  val ch3 = Mux(io.channelEnable(3), io.testPattern, 0.U(16.W))
  val sampleData = Cat(timestamp, ch3, ch2, ch1, ch0)

  val fifo = withReset(reset.asBool || io.fifoClear) {
    Module(new Queue(UInt(128.W), params.fifoDepth, pipe = true))
  }

  val enqueueEvent = sampleEvent && !io.counterClear && !io.fifoClear

  fifo.io.enq.valid := enqueueEvent
  fifo.io.enq.bits := sampleData
  fifo.io.deq.ready := io.fifoPop

  val dropEvent = enqueueEvent && !fifo.io.enq.ready
  val fifoEmpty = fifo.io.count === 0.U
  val fifoFull = fifo.io.count === params.fifoDepth.U
  val fifoHead = Mux(fifo.io.deq.valid, fifo.io.deq.bits, 0.U(128.W))
  val watermarkActive =
    io.fifoWatermark =/= 0.U && fifo.io.count >= io.fifoWatermark

  when (io.counterClear) {
    timer := 0.U
    timestamp := 0.U
    sequence := 0.U
    lfsr := "hace1".U
    freeCounter := 0.U
    sampleCount := 0.U
    dropCount := 0.U
  }.otherwise {
    timestamp := timestamp + 1.U
    freeCounter := freeCounter + 1.U

    when (!io.enable) {
      timer := 0.U
    }.elsewhen (sampleEvent) {
      timer := 0.U
      sequence := sequence + 1.U
      lfsr := lfsrNext
      sampleCount := sampleCount + 1.U
    }.otherwise {
      timer := timer + 1.U
    }

    when (dropEvent) {
      dropCount := dropCount + 1.U
    }
  }

  when (dropEvent) {
    overflow := true.B
  }.elsewhen (io.fifoClear || io.overflowClear) {
    overflow := false.B
  }

  io.fifoLevel := fifo.io.count
  io.dropCount := dropCount
  io.sampleCount := sampleCount
  io.fifoData := fifoHead

  io.fifoEmpty := fifoEmpty
  io.fifoFull := fifoFull
  io.overflow := overflow

  io.irqStatus := Cat(overflow, watermarkActive)
  io.irq := (io.irqEnable & io.irqStatus).orR
}

class DAQAXI4(params: DAQParams, beatBytes: Int)(implicit p: Parameters)
    extends ClockSinkDomain(ClockSinkParameters())(p) {

  private val addressSet = AddressSet(params.address, 4096 - 1)

  val device = new SimpleDevice("daq", Seq("zyy,fpga-daq-v1"))
  val node = AXI4RegisterNode(addressSet, beatBytes = beatBytes)
  val intNode = IntSourceNode(IntSourcePortSimple(num = 1, resources = device.int))

  DTResourceBinding {
    device.reg.head.bind(DTResourceAddress(
      Seq(addressSet),
      DTResourcePermissions(r = true, w = true, x = false, c = false, a = false)))
  }

 override lazy val module = new DAQImpl

 class DAQImpl extends Impl {
    withClockAndReset(clock, reset) {
      val version = "h00010000".U(32.W)

      val capability = Cat(
        params.sampleWidth.U(8.W),
        params.fifoDepth.U(16.W),
        params.channels.U(8.W)
      )

      val enable = RegInit(false.B)
      val samplePeriod = RegInit(1000.U(32.W))
      val channelEnable = RegInit("hf".U(4.W))
      val fifoWatermark = RegInit(math.min(32, params.fifoDepth).U(16.W))
      val irqEnable = RegInit(0.U(2.W))
      val testPattern = RegInit("h55aa".U(16.W))

      val fifoClear = Wire(new DecoupledIO(UInt(1.W)))
      val counterClear = Wire(new DecoupledIO(UInt(1.W)))
      val fifoPop = Wire(new DecoupledIO(UInt(1.W)))
      val irqClear = Wire(new DecoupledIO(UInt(2.W)))

      fifoClear.ready := true.B
      counterClear.ready := true.B
      fifoPop.ready := true.B
      irqClear.ready := true.B

      val fifoClearPulse = fifoClear.valid && fifoClear.bits(0)
      val counterClearPulse = counterClear.valid && counterClear.bits(0)
      val fifoPopPulse = fifoPop.valid && fifoPop.bits(0)
      val overflowClearPulse = irqClear.valid && irqClear.bits(1)

      val core = Module(new DAQCore(params))

      core.io.enable := enable
      core.io.samplePeriod := samplePeriod
      core.io.channelEnable := channelEnable
      core.io.fifoWatermark := fifoWatermark
      core.io.irqEnable := irqEnable
      core.io.testPattern := testPattern

      core.io.fifoClear := fifoClearPulse
      core.io.counterClear := counterClearPulse
      core.io.fifoPop := fifoPopPulse
      core.io.overflowClear := overflowClearPulse

      val status = Cat(
        0.U(28.W),
        core.io.overflow,
        core.io.fifoFull,
        core.io.fifoEmpty,
        enable
      )

      val fifoData0 = core.io.fifoData(31, 0)
      val fifoData1 = core.io.fifoData(63, 32)
      val fifoData2 = core.io.fifoData(95, 64)
      val fifoData3 = core.io.fifoData(127, 96)

      val (irqOut, _) = intNode.out(0)
      irqOut(0) := core.io.irq

      node.regmap(
        0x00 -> Seq(
          RegField.r(32, version)
        ),

        0x04 -> Seq(
          RegField.r(32, capability)
        ),

        0x08 -> Seq(
          RegField(1, enable),
          RegField.w(1, fifoClear),
          RegField.w(1, counterClear),
          RegField.r(29, 0.U(29.W))
        ),

        0x0c -> Seq(
          RegField.r(32, status)
        ),

        0x10 -> Seq(
          RegField(32, samplePeriod)
        ),

        0x14 -> Seq(
          RegField(4, channelEnable),
          RegField.r(28, 0.U(28.W))
        ),

        0x18 -> Seq(
          RegField.r(16, core.io.fifoLevel),
          RegField.r(16, 0.U(16.W))
        ),

        0x1c -> Seq(
          RegField(16, fifoWatermark),
          RegField.r(16, 0.U(16.W))
        ),

        0x20 -> Seq(
          RegField(2, irqEnable),
          RegField.r(30, 0.U(30.W))
        ),

        0x24 -> Seq(
          RegField.r(2, core.io.irqStatus),
          RegField.r(30, 0.U(30.W))
        ),

        0x28 -> Seq(
          RegField.r(32, core.io.dropCount)
        ),

        0x2c -> Seq(
          RegField.r(32, core.io.sampleCount)
        ),

        0x30 -> Seq(
          RegField.r(32, fifoData0)
        ),

        0x34 -> Seq(
          RegField.r(32, fifoData1)
        ),

        0x38 -> Seq(
          RegField.r(32, fifoData2)
        ),

        0x3c -> Seq(
          RegField.r(32, fifoData3)
        ),

        0x40 -> Seq(
          RegField.w(1, fifoPop),
          RegField.r(31, 0.U(31.W))
        ),

        0x44 -> Seq(
          RegField(16, testPattern),
          RegField.r(16, 0.U(16.W))
        ),

        0x48 -> Seq(
          RegField.w(2, irqClear),
          RegField.r(30, 0.U(30.W))
        )
      )
    }
  }
}

trait CanHavePeripheryDAQ {
  this: BaseSubsystem =>

  private val pbus = locateTLBusWrapper(PBUS)

  val daqs = p(PeripheryDAQKey).zipWithIndex.map { case (params, i) =>
    val daq = LazyModule(new DAQAXI4(params, pbus.beatBytes)(p))

    daq.suggestName(s"daq_$i")
    daq.clockNode := pbus.fixedClockNode

    pbus.coupleTo(s"daq_$i") {
      AXI4InwardClockCrossingHelper(
        s"daq_${i}_crossing",
        daq,
        daq.node
      )(SynchronousCrossing()) :=
        AXI4Buffer() :=
        TLToAXI4() :=
        TLFragmenter(
          pbus.beatBytes,
          pbus.blockBytes,
          holdFirstDeny = true
        ) := _
    }

    ibus.fromSync := daq.intNode

    daq
  }
}
