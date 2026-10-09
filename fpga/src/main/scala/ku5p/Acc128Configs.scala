package chipyard.fpga.ku5p

import org.chipsalliance.cde.config.Config
import java.nio.charset.StandardCharsets
import java.nio.file.{Files, Paths}

// Independent capacity experiment: same WS16 datapath and SPAD as Layer.
// Do not overwrite gemmini_params.h when elaborating this configuration.
object Ku5pAcc128GemminiConfig {
  val config = Ku5pGemminiConfigs.inferenceConfig.copy(
    has_layer_matmul = true,
    acc_capacity = gemmini.CapacityInKilobytes(128),
    headerFileName = "gemmini_params_acc128.h")
  require(config.meshRows == 16 && config.meshColumns == 16)
  require(config.acc_banks * config.acc_bank_entries == 2048)
  require(config.sp_banks * config.sp_bank_entries == 16384)
}

class GemminiLayerAcc128RocketKu5pConfig extends Config(
  new gemmini.DefaultGemminiConfig(Ku5pAcc128GemminiConfig.config) ++
  new WithKu5pPMU(8) ++ new WithKu5pTweaks ++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(nWays = 4, capacityKB = 128) ++
  new chipyard.config.WithSystemBusWidth(128) ++ new chipyard.RocketConfig)

class GemminiLayerAcc128RocketKu5pSimConfig extends Config(
  new gemmini.DefaultGemminiConfig(Ku5pAcc128GemminiConfig.config) ++
  new WithKu5pPMU(8) ++ new WithKu5pSimPeripherals ++
  new freechips.rocketchip.subsystem.WithNBanks(1) ++
  new freechips.rocketchip.subsystem.WithInclusiveCache(nWays = 4, capacityKB = 128) ++
  new chipyard.config.WithSystemBusWidth(128) ++ new chipyard.RocketConfig)

// Generate the software ABI from the SAME Scala config, without running any
// simulation or elaborating a SoC. The normal elaborator emits identical text.
object GenerateKu5pAcc128Header extends App {
  require(args.length == 1, "usage: GenerateKu5pAcc128Header <output-header>")
  val path = Paths.get(args(0)).toAbsolutePath
  Files.createDirectories(path.getParent)
  Files.write(path, Ku5pAcc128GemminiConfig.config.generateHeader()
    .getBytes(StandardCharsets.UTF_8))
  println(s"[BUILD] WS16/SPAD256KiB/ACC128KiB header: $path")
}
