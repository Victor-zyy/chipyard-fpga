// See LICENSE for license details.
package chipyard.fpga.microphasea7

import chisel3._

import freechips.rocketchip.jtag.{JTAGIO}
import freechips.rocketchip.subsystem.{PeripheryBusKey}
import freechips.rocketchip.tilelink.{TLBundle}
import freechips.rocketchip.util.{HeterogeneousBag}
import freechips.rocketchip.diplomacy.{LazyRawModuleImp}
import org.chipsalliance.diplomacy.nodes.{HeterogeneousBag}

import sifive.blocks.devices.uart.{UARTPortIO, UARTParams}
import sifive.blocks.devices.jtag.{JTAGPins, JTAGPinsFromPort}
import sifive.blocks.devices.pinctrl.{BasePin}

//import sifive.fpgashells.ip.xilinx.{IBUFG, IOBUF, PULLUP, PowerOnResetFPGAOnly}
import sifive.fpgashells.shell._
import sifive.fpgashells.ip.xilinx._
import sifive.fpgashells.shell.xilinx._
import sifive.fpgashells.clocks._

import chipyard._
import chipyard.harness._
import chipyard.iobinders._
import testchipip.serdes._

/*** UART ***/
class WithMicrophaseA7UARTTSI(uartBaudRate: BigInt = 115200) extends HarnessBinder({
  case (th: HasHarnessInstantiators, port: UARTTSIPort, chipId: Int) => {
    val microphasea7th = th.asInstanceOf[LazyRawModuleImp].wrapper.asInstanceOf[MicrophaseA7Harness]
    microphasea7th.io_uart_bb.bundle <> port.io.uart
  }
})

class WithMicrophaseA7StandardUART extends HarnessBinder({
  case (th: HasHarnessInstantiators, port: UARTPort, chipId: Int) => {
    val microphasea7th = th.asInstanceOf[LazyRawModuleImp].wrapper.asInstanceOf[MicrophaseA7Harness]
    microphasea7th.io_uart_bb.bundle <> port.io
  }
})

/*** SPI ***/
class WithMicrophaseA7SDCard extends HarnessBinder({
  case (th: HasHarnessInstantiators, port: SPIPort, chipId: Int) => {
    val microphasea7th = th.asInstanceOf[LazyRawModuleImp].wrapper.asInstanceOf[MicrophaseA7Harness]
    microphasea7th.io_spi_bb.get.bundle <> port.io
  }
})
/*** DDR ***/
class WithMicrophaseA7DDRTL extends HarnessBinder({
  case (th: HasHarnessInstantiators, port: TLMemPort, chipId: Int) => {
    val microphaseTh = th.asInstanceOf[LazyRawModuleImp].wrapper.asInstanceOf[MicrophaseA7Harness]
    val bundles = microphaseTh.ddrClient.get.out.map(_._1)
    val ddrClientBundle = Wire(new HeterogeneousBag(bundles.map(_.cloneType)))
    bundles.zip(ddrClientBundle).foreach { case (bundle, io) => bundle <> io }
    ddrClientBundle <> port.io
  }
})

