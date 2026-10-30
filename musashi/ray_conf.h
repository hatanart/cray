/* Musashi configuration for the RAY board: a plain 68000.
 * Used through -DMUSASHI_CNF='"ray_conf.h"'. */
#ifndef RAY_CONF_H
#define RAY_CONF_H

#define OPT_OFF                     0
#define OPT_ON                      1
#define OPT_SPECIFY_HANDLER         2

#define M68K_COMPILE_FOR_MAME       OPT_OFF

#define M68K_EMULATE_010            OPT_OFF
#define M68K_EMULATE_EC020          OPT_OFF
#define M68K_EMULATE_020            OPT_OFF
#define M68K_EMULATE_030            OPT_OFF
#define M68K_EMULATE_040            OPT_OFF

#define M68K_SEPARATE_READS         OPT_OFF
#define M68K_SIMULATE_PD_WRITES     OPT_OFF
#define M68K_EMULATE_INT_ACK        OPT_OFF
#define M68K_EMULATE_BKPT_ACK       OPT_OFF
#define M68K_EMULATE_TRACE          OPT_OFF
#define M68K_CMPILD_HAS_CALLBACK    OPT_OFF
#define M68K_RTE_HAS_CALLBACK       OPT_OFF
#define M68K_TAS_HAS_CALLBACK       OPT_OFF
#define M68K_ILLG_HAS_CALLBACK      OPT_OFF
#define M68K_EMULATE_FC             OPT_OFF
#define M68K_MONITOR_PC             OPT_OFF
#define M68K_EMULATE_PREFETCH       OPT_OFF
#define M68K_EMULATE_ADDRESS_ERROR  OPT_OFF
#define M68K_ALINE_HOOK             OPT_OFF
#define M68K_LOG_ENABLE             OPT_OFF
#define M68K_LOG_1010_1111          OPT_OFF
#define M68K_EMULATE_PMMU           OPT_OFF
#define M68K_USE_64_BIT             OPT_ON

/* RESET instruction: pulses the peripheral reset line (and the watchdog) */
#define M68K_EMULATE_RESET          OPT_SPECIFY_HANDLER
#define M68K_RESET_CALLBACK()       ray_reset_instr()

/* Instruction hook, only armed while a ROM routine is called from C */
#define M68K_INSTRUCTION_HOOK       OPT_SPECIFY_HANDLER
#define M68K_INSTRUCTION_CALLBACK(pc) do { if (ray_hook_armed) ray_instr_hook(pc); } while (0)

extern int ray_hook_armed;
void ray_reset_instr(void);
void ray_instr_hook(unsigned int pc);

#endif
