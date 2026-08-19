package chipyard.fpga.ku5p

import chisel3._
import org.chipsalliance.diplomacy.nodes.HeterogeneousBag

import chipyard.harness.HarnessBinder
import chipyard.iobinders.{SPIPort, TLMemPort, UARTPort}

class WithKu5pUART extends HarnessBinder({
  case (th: Ku5pHarnessImp, port: UARTPort, chipId: Int) =>
    th.ku5pOuter.ioUART.bundle <> port.io
})

class WithKu5pSPISDCard extends HarnessBinder({
  case (th: Ku5pHarnessImp, port: SPIPort, chipId: Int) =>
    th.ku5pOuter.ioSPI.bundle <> port.io
})

class WithKu5pDDRMem extends HarnessBinder({
  case (th: Ku5pHarnessImp, port: TLMemPort, chipId: Int) =>
    val bundles = th.ku5pOuter.ddrClient.out.map(_._1)
    val ddrClientBundle = Wire(new HeterogeneousBag(bundles.map(_.cloneType)))
    bundles.zip(ddrClientBundle).foreach { case (bundle, io) => bundle <> io }
    ddrClientBundle <> port.io
})
