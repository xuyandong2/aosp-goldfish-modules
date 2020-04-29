/* Put all options we expect gki_defconfig to provide us here */

#ifndef CONFIG_CFG80211
#error CONFIG_CFG80211 is required
#endif

#ifndef CONFIG_NAMESPACES
#error CONFIG_NAMESPACES is required
#endif

#ifndef CONFIG_PCI
#error CONFIG_PCI is required
#endif

#ifndef CONFIG_ZRAM
#error CONFIG_ZRAM is required
#endif

#ifndef CONFIG_ZSMALLOC
#error CONFIG_ZSMALLOC is required
#endif

#ifndef CONFIG_COMPAT
#error CONFIG_COMPAT is required
#endif
