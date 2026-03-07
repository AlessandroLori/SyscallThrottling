#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xa61fd7aa, "__check_object_size" },
	{ 0xd5ad82a1, "misc_deregister" },
	{ 0x0040afbe, "param_ops_ulong" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0xd710adbf, "__kmalloc_noprof" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0x2352b148, "timer_delete_sync" },
	{ 0xe1e1f979, "_raw_spin_lock_irqsave" },
	{ 0xd272d446, "__fentry__" },
	{ 0xe8213e80, "_printk" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0x32feeafc, "mod_timer" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xf46d5bf3, "mutex_lock" },
	{ 0x2719b9fa, "const_current_task" },
	{ 0xc1e6c71e, "__mutex_init" },
	{ 0x81a1a811, "_raw_spin_unlock_irqrestore" },
	{ 0xaca12394, "misc_register" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x092a35a2, "_copy_to_user" },
	{ 0x058c185a, "jiffies" },
	{ 0xf46d5bf3, "mutex_unlock" },
	{ 0xc064623f, "__kmalloc_cache_noprof" },
	{ 0x546c19d9, "validate_usercopy_range" },
	{ 0x02f9bbf0, "timer_init_key" },
	{ 0xe4de56b4, "__ubsan_handle_load_invalid_value" },
	{ 0xfaabfe5e, "kmalloc_caches" },
	{ 0xbebe66ff, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xa61fd7aa,
	0xd5ad82a1,
	0x0040afbe,
	0x092a35a2,
	0xd710adbf,
	0xcb8b6ec6,
	0x2352b148,
	0xe1e1f979,
	0xd272d446,
	0xe8213e80,
	0xbd03ed67,
	0xd272d446,
	0x90a48d82,
	0x32feeafc,
	0xbd03ed67,
	0xf46d5bf3,
	0x2719b9fa,
	0xc1e6c71e,
	0x81a1a811,
	0xaca12394,
	0xd272d446,
	0x092a35a2,
	0x058c185a,
	0xf46d5bf3,
	0xc064623f,
	0x546c19d9,
	0x02f9bbf0,
	0xe4de56b4,
	0xfaabfe5e,
	0xbebe66ff,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"__check_object_size\0"
	"misc_deregister\0"
	"param_ops_ulong\0"
	"_copy_from_user\0"
	"__kmalloc_noprof\0"
	"kfree\0"
	"timer_delete_sync\0"
	"_raw_spin_lock_irqsave\0"
	"__fentry__\0"
	"_printk\0"
	"__ref_stack_chk_guard\0"
	"__stack_chk_fail\0"
	"__ubsan_handle_out_of_bounds\0"
	"mod_timer\0"
	"random_kmalloc_seed\0"
	"mutex_lock\0"
	"const_current_task\0"
	"__mutex_init\0"
	"_raw_spin_unlock_irqrestore\0"
	"misc_register\0"
	"__x86_return_thunk\0"
	"_copy_to_user\0"
	"jiffies\0"
	"mutex_unlock\0"
	"__kmalloc_cache_noprof\0"
	"validate_usercopy_range\0"
	"timer_init_key\0"
	"__ubsan_handle_load_invalid_value\0"
	"kmalloc_caches\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "450B215775D6C763E1B32D2");
