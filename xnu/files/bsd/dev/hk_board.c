/*
 * The board's addresses (sys/hk_board.h): board_<block>=0x... on the command line, else the address on
 * the phone the port was brought up on. Read where a driver maps its block, so that one kernel serves every
 * phone whose SoC has the same blocks, wherever its device tree puts them.
 *
 * The names, and the drivers that ask for them (the default is the POCO X5 Pro's, SM7325):
 *   epss       cpufreq: the CPU frequency domains (qcom,cpufreq-epss "freq-domain0")      0x18591000
 *   cmd_db     cpufreq: RPMh's command database                                         0x80860000
 *   rsc        cpufreq: the apps RSC (RPMh votes)                                       0x18220000
 *   tsens0/1   thermal: the TM regions of the two TSENS blocks                         0x0c263000, 0x0c265000
 *   gcc        display: the global clock controller                                     0x00100000
 *   dispcc     display: the display clock controller                                    0x0af00000
 *   intf       display: the DSI panel's timing engine (mdp_intf_base)                   0x0ae36000
 *   ufshc      ufs: the UFS host controller ("ufs_mem")                                 0x01d84000
 *   spmi_core, spmi_cnfg, spmi_obsrvr   buttons: the PMIC arbiter                       0x0c440000 ...
 */
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/hk_board.h>
#include <sys/systm.h>

vm_offset_t
hk_board_addr(const char *name, vm_offset_t dflt)
{
	char arg[40] = "board_";
	uint64_t value = 0;

	strlcat(arg, name, sizeof(arg));
	if (PE_parse_boot_argn(arg, &value, sizeof(value)) && value != 0) {
		return (vm_offset_t)value;
	}
	return dflt;
}

uint32_t
hk_board_number(const char *name, uint32_t dflt)
{
	char arg[40] = "board_";
	uint32_t value = 0;

	strlcat(arg, name, sizeof(arg));
	if (PE_parse_boot_argn(arg, &value, sizeof(value))) {
		return value;
	}
	return dflt;
}

void
hk_board_report(void)
{
	static const char *const names[] = { "epss", "cmd_db", "rsc", "tsens0", "tsens1", "gcc", "dispcc", "intf", "ufshc",
		                             "spmi_core", "spmi_cnfg", "spmi_obsrvr" };
	int given = 0;

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		vm_offset_t a = hk_board_addr(names[i], 0);

		if (a != 0) {
			printf("board: %s at %#lx (from the command line)\n", names[i], (unsigned long)a);
			given++;
		}
	}
	if (given == 0) {
		printf("board: no board_ addresses given: the POCO X5 Pro's (SM7325) for every block\n");
	}
}
