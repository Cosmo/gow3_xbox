/* Unported services remain unresolved: the native loader reports the exact first import.
 * These are host link adapters, not successful guest implementations. */
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#define UNPORTED(name) uintptr_t runtime_##name##_resolve(const char *nid) { (void)nid; return 0; }
UNPORTED(audio)
UNPORTED(ajm)
UNPORTED(videodec)
UNPORTED(pad)
void runtime_audio_report(void) { puts("UWP milestone: audio not ported"); }
void runtime_ajm_report(void) { puts("UWP milestone: media decoding not ported"); }
void runtime_pad_report(void) { puts("UWP milestone: guest controller API not ported"); }
int runtime_pad_describe(void) { return 0; }
void gow3gpu_register_kernel(void) { puts("UWP milestone: GPU and GPU event queues not ported"); }
int gow3gpu_init(const Gow3GpuConfig *config) { (void)config; return -1; }
uintptr_t gow3gpu_resolve(const char *nid) { (void)nid; return 0; }
int gow3gpu_handle_fault(void *context,void *address) { (void)context; (void)address; return 0; }
void gow3gpu_dump_guest_writes(void *context) { (void)context; }
unsigned gow3gpu_symbol_count(void) { return 0; }
int gow3gpu_text_input_begin(const char *text,const char *prompt) { (void)text; (void)prompt; return 0; }
int gow3gpu_text_input_poll(char *text,uint64_t size) { (void)text; (void)size; return 2; }
int gow3gpu_choice_begin(const char *title,const char *const *items,const char *const *images,int count,int focus) { (void)title; (void)items; (void)images; (void)count; (void)focus; return 0; }
int gow3gpu_choice_poll(void) { return -2; }
int gow3gpu_overlay_captures_input(void) { return 0; }
int vulkan_smoke(void) { fputs("UWP milestone: Vulkan backend unavailable\n",stderr); return 21; }
/* Optional overlay hooks are never installed in --cpu-only mode. */
float ABI gow3_orbs_gain(void *player,float value) { (void)player; return value; }
void gow3_orbs_status(int supported) { (void)supported; }
uintptr_t gow3_cheat_flag(unsigned id) { (void)id; return 0; }
void gow3_cheat_status(unsigned id) { (void)id; }
uintptr_t gow3_actor_slots_address(void) { return 0; }
void gow3_actor_status(int supported) { (void)supported; }
uintptr_t gow3_damage_multiplier(int taken) { (void)taken; return 0; }
uintptr_t gow3_orb_pickup_multiplier(unsigned kind) { (void)kind; return 0; }
void gow3_stat_multipliers_status(int damage,int pickup) { (void)damage; (void)pickup; }
