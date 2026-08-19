#########################################################################################
# fpga prototype makefile
#########################################################################################

#########################################################################################
# general path variables
#########################################################################################
base_dir=$(abspath ..)
sim_dir=$(abspath .)

# do not generate simulation files
sim_name := none

#########################################################################################
# include shared variables
#########################################################################################
#SUB_PROJECT ?= vcu118
SUB_PROJECT ?= microphasea7

ifeq ($(SUB_PROJECT),vc707)
	SBT_PROJECT       ?= chipyard_fpga
	MODEL             ?= VC707FPGATestHarness
	VLOG_MODEL        ?= VC707FPGATestHarness
	MODEL_PACKAGE     ?= chipyard.fpga.vc707
	CONFIG            ?= RocketVC707Config
	CONFIG_PACKAGE    ?= chipyard.fpga.vc707
	GENERATOR_PACKAGE ?= chipyard
	TB                ?= none # unused
	TOP               ?= ChipTop
	BOARD             ?= vc707
	FPGA_BRAND        ?= xilinx
endif

ifeq ($(SUB_PROJECT),vcu118)
	SBT_PROJECT       ?= chipyard_fpga
	MODEL             ?= VCU118FPGATestHarness
	VLOG_MODEL        ?= VCU118FPGATestHarness
	MODEL_PACKAGE     ?= chipyard.fpga.vcu118
	CONFIG            ?= RocketVCU118Config
	CONFIG_PACKAGE    ?= chipyard.fpga.vcu118
	GENERATOR_PACKAGE ?= chipyard
	TB                ?= none # unused
	TOP               ?= ChipTop
	BOARD             ?= vcu118
	FPGA_BRAND        ?= xilinx
endif

ifeq ($(SUB_PROJECT),nexysvideo)
	SBT_PROJECT       ?= chipyard_fpga
	MODEL             ?= NexysVideoHarness
	VLOG_MODEL        ?= NexysVideoHarness
	MODEL_PACKAGE     ?= chipyard.fpga.nexysvideo
	CONFIG            ?= RocketNexysVideoConfig
	CONFIG_PACKAGE    ?= chipyard.fpga.nexysvideo
	GENERATOR_PACKAGE ?= chipyard
	TB                ?= none # unused
	TOP               ?= ChipTop
	BOARD             ?= nexys_video
	FPGA_BRAND        ?= xilinx
endif

ifeq ($(SUB_PROJECT),arty35t)
	# TODO: Fix with Arty
	SBT_PROJECT       ?= chipyard_fpga
	MODEL             ?= Arty35THarness
	VLOG_MODEL        ?= Arty35THarness
	MODEL_PACKAGE     ?= chipyard.fpga.arty
	CONFIG            ?= TinyRocketArtyConfig
	CONFIG_PACKAGE    ?= chipyard.fpga.arty
	GENERATOR_PACKAGE ?= chipyard
	TB                ?= none # unused
	TOP               ?= ChipTop
	BOARD             ?= arty
	FPGA_BRAND        ?= xilinx
endif
ifeq ($(SUB_PROJECT),arty100t)
	# TODO: Fix with Arty
	SBT_PROJECT       ?= chipyard_fpga
	MODEL             ?= Arty100THarness
	VLOG_MODEL        ?= Arty100THarness
	MODEL_PACKAGE     ?= chipyard.fpga.arty100t
	CONFIG            ?= RocketArty100TConfig
	CONFIG_PACKAGE    ?= chipyard.fpga.arty100t
	GENERATOR_PACKAGE ?= chipyard
	TB                ?= none # unused
	TOP               ?= ChipTop
	BOARD             ?= arty_a7_100
	FPGA_BRAND        ?= xilinx
endif
ifeq ($(SUB_PROJECT),datastorm)
	SBT_PROJECT       ?= chipyard_fpga
	MODEL             ?= DatastormHarness
	VLOG_MODEL        ?= DatastormHarness
	MODEL_PACKAGE     ?= chipyard.fpga.datastorm
	CONFIG            ?= RocketDatastormConfig
	CONFIG_PACKAGE    ?= chipyard.fpga.datastorm
	GENERATOR_PACKAGE ?= chipyard
	TB                ?= none # unused
	TOP               ?= ChipTop
	BOARD			  ?= trenz_datastorm
	FPGA_BRAND        ?= altera
endif

ifeq ($(SUB_PROJECT),microphasea7)
	SBT_PROJECT       ?= chipyard_fpga
	MODEL             ?= MicrophaseA7Harness
	VLOG_MODEL        ?= MicrophaseA7Harness
	MODEL_PACKAGE     ?= chipyard.fpga.microphasea7
	CONFIG            ?= RocketMicrophaseA7Config
	CONFIG_PACKAGE    ?= chipyard.fpga.microphasea7
	GENERATOR_PACKAGE ?= chipyard
	TB                ?= none # unused
	TOP               ?= ChipTop
	BOARD			  ?= microphasea7
	FPGA_BRAND        ?= xilinx
endif

ifeq ($(SUB_PROJECT),ku5p)
	SBT_PROJECT       ?= chipyard_fpga
	MODEL             ?= Ku5pHarness
	VLOG_MODEL        ?= Ku5pHarness
	MODEL_PACKAGE     ?= chipyard.fpga.ku5p
	CONFIG            ?= RocketKu5pConfig
	CONFIG_PACKAGE    ?= chipyard.fpga.ku5p
	GENERATOR_PACKAGE ?= chipyard
	TB                ?= none # unused
	TOP               ?= ChipTop
	BOARD             ?= ku5p
	FPGA_BRAND        ?= xilinx
endif

export USE_CHISEL6=1

include $(base_dir)/variables.mk

# default variables to build the arty example
# setup the board to use

.PHONY: default
default: $(mcs)

#########################################################################################
# misc. directories
#########################################################################################
fpga_dir := $(base_dir)/fpga/fpga-shells/$(FPGA_BRAND)
fpga_common_script_dir := $(fpga_dir)/common/tcl

#########################################################################################
# setup misc. sim files
#########################################################################################
# copy files but ignore *.h files in *.f (match vcs)
$(sim_files): $(SIM_FILE_REQS) $(ALL_MODS_FILELIST) | $(GEN_COLLATERAL_DIR)
	-cp -f $(SIM_FILE_REQS) $(GEN_COLLATERAL_DIR)
	touch $@
	$(foreach file,\
		$(SIM_FILE_REQS),\
		$(if $(filter %.h,$(file)),\
			,\
			echo "$(addprefix $(GEN_COLLATERAL_DIR)/, $(notdir $(file)))" >> $@;))

#########################################################################################
# import other necessary rules and variables
#########################################################################################
include $(base_dir)/common.mk

#########################################################################################
# copy from other directory
#########################################################################################
all_vsrcs := \
	$(base_dir)/generators/rocket-chip-blocks/vsrc/SRLatch.v

#########################################################################################
# vivado rules
#########################################################################################
# combine all sources into single .f
synth_list_f := $(build_dir)/$(long_name).vsrcs.f
$(synth_list_f): $(sim_common_files) $(all_vsrcs)
	rm -f $@
	$(foreach file,$(all_vsrcs),echo "$(file)" >> $@;)
	cat $(sim_common_files) >> $@

ifeq ($(FPGA_BRAND),xilinx)
BIT_FILE := $(build_dir)/obj/$(MODEL).bit
BIN_FILE := $(build_dir)/obj/$(MODEL).bin
xilinx_flow_files := \
	$(fpga_common_script_dir)/vivado.tcl \
	$(fpga_common_script_dir)/prologue.tcl \
	$(fpga_common_script_dir)/bitstream.tcl
xilinx_board_files := \
	$(wildcard $(fpga_dir)/$(BOARD)/tcl/*.tcl) \
	$(wildcard $(fpga_dir)/$(BOARD)/constraints/*.tcl) \
	$(wildcard $(fpga_dir)/$(BOARD)/constraints/*.xdc)
$(BIT_FILE): $(synth_list_f) $(xilinx_flow_files) $(xilinx_board_files)
	cd $(build_dir); vivado \
		-nojournal -mode batch \
		-source $(fpga_common_script_dir)/vivado.tcl \
		-tclargs \
			-top-module "$(MODEL)" \
			-F "$(synth_list_f)" \
			-board "$(BOARD)" \
			-ip-vivado-tcls "$(shell find '$(build_dir)' -name '*.vivado.tcl')"
endif
ifeq ($(FPGA_BRAND),altera)
BIT_FILE := $(build_dir)/output_files/$(MODEL).qsf
$(BIT_FILE): $(synth_list_f)
	cd $(build_dir); quartus_sh \
		-t $(fpga_common_script_dir)/quartus.tcl \
		"$(MODEL)" \
		"$(BOARD)" \
		"$(long_name)" \
		"$(shell find '$(build_dir)' -name '*.quartus.tcl')"
endif

.PHONY: bitstream
bitstream: $(BIT_FILE)

ifeq ($(SUB_PROJECT),ku5p)
KU5P_CFGMEM_BIN_TCL := $(fpga_common_script_dir)/write_cfgmem_bin.tcl

$(BIN_FILE): $(BIT_FILE) $(KU5P_CFGMEM_BIN_TCL)
	cd $(build_dir); vivado \
		-nojournal -mode batch \
		-source $(KU5P_CFGMEM_BIN_TCL) \
		-tclargs "$(BIT_FILE)" "$(BIN_FILE)" spix4 16

.PHONY: flash-bin
flash-bin: $(BIN_FILE)
	@test -f "$(BIN_FILE)" || { echo "Missing configuration image: $(BIN_FILE)"; exit 1; }
	@echo "KU5P SPI flash image: $(BIN_FILE)"
endif

.PHONY: debug-bitstream
debug-bitstream: $(build_dir)/obj/post_synth.dcp
	cd $(build_dir); vivado \
		-nojournal -mode batch \
		-source $(sim_dir)/scripts/run_impl_bitstream.tcl \
		-tclargs \
			$(build_dir)/obj/post_synth.dcp \
			$(BOARD) \
			$(build_dir)/debug_obj \
			$(fpga_common_script_dir)

#########################################################################################
# general cleanup rules
#########################################################################################
.PHONY: clean
clean:
	rm -rf $(gen_dir)
