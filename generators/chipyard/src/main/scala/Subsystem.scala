//******************************************************************************
// Copyright (c) 2019 - 2019, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE and LICENSE.SiFive for license details.
//------------------------------------------------------------------------------

package chipyard

import chisel3._

import freechips.rocketchip.prci._
import org.chipsalliance.cde.config.{Field, Parameters}
import freechips.rocketchip.devices.tilelink._
import freechips.rocketchip.devices.debug.{HasPeripheryDebug, ExportDebug, DebugModuleKey}
import sifive.blocks.devices.uart.{HasPeripheryUART, PeripheryUARTKey}
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.tile._
import freechips.rocketchip.tilelink._
import freechips.rocketchip.interrupts._
import freechips.rocketchip.util._
import freechips.rocketchip.subsystem._
import freechips.rocketchip.amba.axi4._

import testchipip.serdes.{CanHavePeripheryTLSerial, SerialTLKey}

trait CanHaveHTIF { this: BaseSubsystem =>
  // Advertise HTIF if system can communicate with fesvr
  if (this match {
    case _: CanHavePeripheryTLSerial if (p(SerialTLKey).size != 0) => true
    case _: HasPeripheryDebug if (!p(DebugModuleKey).isEmpty && p(ExportDebug).dmi) => true
    case _ => false
  }) {
    ResourceBinding {
      val htif = new Device {
        def describe(resources: ResourceBindings): Description = {
          val compat = resources("compat").map(_.value)
          Description("htif", Map(
            "compatible" -> compat))
        }
      }
      Resource(htif, "compat").bind(ResourceString("ucb,htif0"))
    }
  }
}

// This trait adds the "chosen" node to DTS, which
// can be used to pass information to OS about the earlycon
case object ChosenInDTS extends Field[Boolean](true)
trait CanHaveChosenInDTS { this: BaseSubsystem =>
  if (p(ChosenInDTS)) {
    this match {
      case t: HasPeripheryUART if (!p(PeripheryUARTKey).isEmpty) => {
        val chosen = new Device {
          def describe(resources: ResourceBindings): Description = {
            val stdout = resources("stdout").map(_.value)
            Description("chosen", resources("uart").headOption.map { case Binding(_, value) =>
              "stdout-path" -> Seq(value)
            }.toMap)
          }
        }
        ResourceBinding {
          t.uarts.foreach(u => Resource(chosen, "uart").bind(ResourceAlias(u.device.label)))
        }
      }
      case _ =>
    }
  }
}

case class PMUEventToMhpmevent(eventIdx: BigInt, selector: BigInt)
case class PMUEventToMhpmcounters(eventIdxStart: BigInt, eventIdxEnd: BigInt, counterMask: BigInt)
case class PMURawEventToMhpmcounters(selector: BigInt, selectorMask: BigInt, counterMask: BigInt)

case class PMUDeviceTreeParams(
  eventToMhpmevent: Seq[PMUEventToMhpmevent],
  eventToMhpmcounters: Seq[PMUEventToMhpmcounters],
  rawEventToMhpmcounters: Seq[PMURawEventToMhpmcounters])

/** Optional root-level `riscv,pmu` node consumed by OpenSBI's generic platform. */
case object PMUDeviceTreeKey extends Field[Option[PMUDeviceTreeParams]](None)

trait CanHavePMUInDTS { this: BaseSubsystem =>
  p(PMUDeviceTreeKey).foreach { params =>
    val maxU32 = (BigInt(1) << 32) - 1
    val maxU64 = (BigInt(1) << 64) - 1

    def u32(value: BigInt): ResourceInt = {
      require(value >= 0 && value <= maxU32, s"PMU DTS cell does not fit in 32 bits: $value")
      ResourceInt(value)
    }

    def u64(value: BigInt): Seq[ResourceValue] = {
      require(value >= 0 && value <= maxU64, s"PMU DTS value does not fit in 64 bits: $value")
      Seq(u32(value >> 32), u32(value & maxU32))
    }

    val pmu = new Device {
      def describe(resources: ResourceBindings): Description = {
        val eventToMhpmevent = params.eventToMhpmevent.flatMap { event =>
          Seq(u32(event.eventIdx)) ++ u64(event.selector)
        }
        val eventToMhpmcounters = params.eventToMhpmcounters.flatMap { eventRange =>
          Seq(
            u32(eventRange.eventIdxStart),
            u32(eventRange.eventIdxEnd),
            u32(eventRange.counterMask))
        }
        val rawEventToMhpmcounters = params.rawEventToMhpmcounters.flatMap { rawRange =>
          u64(rawRange.selector) ++ u64(rawRange.selectorMask) ++ Seq(u32(rawRange.counterMask))
        }

        Description("pmu", Map(
          "compatible" -> Seq(ResourceString("riscv,pmu")),
          "riscv,event-to-mhpmevent" -> eventToMhpmevent,
          "riscv,event-to-mhpmcounters" -> eventToMhpmcounters,
          "riscv,raw-event-to-mhpmcounters" -> rawEventToMhpmcounters).filterNot(_._2.isEmpty))
      }
    }

    ResourceBinding {
      Resource(pmu, "exists").bind(ResourceString("yes"))
    }
  }
}

class ChipyardSubsystem(implicit p: Parameters) extends BaseSubsystem
    with InstantiatesHierarchicalElements
    with HasTileNotificationSinks
    with HasTileInputConstants
    with CanHavePeripheryCLINT
    with CanHavePeripheryPLIC
    with HasPeripheryDebug
    with HasHierarchicalElementsRootContext
    with HasHierarchicalElements
    with CanHaveHTIF
    with CanHaveChosenInDTS
    with CanHavePMUInDTS
{
  def coreMonitorBundles = totalTiles.values.map {
    case r: RocketTile => r.module.core.rocketImpl.coreMonitorBundle
    case b: boom.v3.common.BoomTile => b.module.core.coreMonitorBundle
    case b: boom.v4.common.BoomTile => b.module.core.coreMonitorBundle
  }.toList

  // No-tile configs have to be handled specially.
  if (totalTiles.size == 0) {
    // no PLIC, so sink interrupts to nowhere
    require(!p(PLICKey).isDefined)
    val intNexus = IntNexusNode(sourceFn = x => x.head, sinkFn = x => x.head)
    val intSink = IntSinkNode(IntSinkPortSimple())
    intSink := intNexus :=* ibus.toPLIC

    // avoids a bug when there are no interrupt sources
    ibus { ibus.fromAsync := NullIntSource() }

    // Need to have at least 1 driver to the tile notification sinks
    tileHaltXbarNode := IntSourceNode(IntSourcePortSimple())
    tileWFIXbarNode := IntSourceNode(IntSourcePortSimple())
    tileCeaseXbarNode := IntSourceNode(IntSourcePortSimple())
  }

  // Relying on [[TLBusWrapperConnection]].driveClockFromMaster for
  // bus-couplings that are not asynchronous strips the bus name from the sink
  // ClockGroup. This makes it impossible to determine which clocks are driven
  // by which bus based on the member names, which is problematic when there is
  // a rational crossing between two buses. Instead, provide all bus clocks
  // directly from the allClockGroupsNode in the subsystem to ensure bus
  // names are always preserved in the top-level clock names.
  //
  // For example, using a RationalCrossing between the Sbus and Cbus, and
  // driveClockFromMaster = Some(true) results in all cbus-attached device and
  // bus clocks to be given names of the form "subsystem_sbus_[0-9]*".
  // Conversly, if an async crossing is used, they instead receive names of the
  // form "subsystem_cbus_[0-9]*". The assignment below provides the latter names in all cases.
  Seq(PBUS, FBUS, MBUS, CBUS).foreach { loc =>
    tlBusWrapperLocationMap.lift(loc).foreach { _.clockGroupNode := allClockGroupsNode }
  }
  override lazy val module = new ChipyardSubsystemModuleImp(this)
}

class ChipyardSubsystemModuleImp[+L <: ChipyardSubsystem](_outer: L) extends BaseSubsystemModuleImp(_outer)
    with HasHierarchicalElementsRootContextModuleImp {
  override lazy val outer = _outer
}
