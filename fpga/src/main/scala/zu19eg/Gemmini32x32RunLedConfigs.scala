package chipyard.fpga.zu19eg

import org.chipsalliance.cde.config.Config
import freechips.rocketchip.tile.RoCCRunLedDebugKey

/** Enable only observation registers and LED routing. No geometry, capacity,
  * arbitration, command-queue, clock-frequency or software ABI change.
  */
class WithRoCCRunLedDebug extends Config((site, here, up) => {
  case RoCCRunLedDebugKey => true
})

class GemminiRocketZu19eg32x32Sp512Acc128100MHzScalePipeRunLedConfig extends Config(
  new WithRoCCRunLedDebug ++
  new GemminiRocketZu19eg32x32Sp512Acc128100MHzScalePipeConfig)
