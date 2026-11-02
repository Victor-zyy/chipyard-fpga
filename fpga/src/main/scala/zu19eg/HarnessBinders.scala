package chipyard.fpga.zu19eg

import chisel3._
import org.chipsalliance.diplomacy.nodes.HeterogeneousBag

import chipyard.harness.HarnessBinder
import chipyard.iobinders.{SPIPort, TLMemPort, UARTPort}

class WithZu19egUART extends HarnessBinder({
  case (th: Zu19egHarnessImp, port: UARTPort, chipId: Int) =>
    th.zu19egOuter.ioUART.bundle <> port.io
})

class WithZu19egSPISDCard extends HarnessBinder({
  case (th: Zu19egHarnessImp, port: SPIPort, chipId: Int) =>
    th.zu19egOuter.ioSPI.bundle <> port.io
})

class WithZu19egDDRMem extends HarnessBinder({
  case (th: Zu19egHarnessImp, port: TLMemPort, chipId: Int) =>
    val bundles = th.zu19egOuter.ddrClient.out.map(_._1)
    val ddrClientBundle = Wire(new HeterogeneousBag(bundles.map(_.cloneType)))
    bundles.zip(ddrClientBundle).foreach { case (bundle, io) => bundle <> io }
    ddrClientBundle <> port.io
})
