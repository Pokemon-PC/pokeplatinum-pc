#if defined(PLATFORM_PC)
    /* PC port: the DS overlay loader runs the .sinit entries of an overlay
     * when it loads it. Here each NitroStaticInit registers itself at
     * start-up with the overlay its file belongs to (PC_OVERLAY_INDEX, from
     * the per-overlay header of tools/pc_port/gen_overlay_sections.py) and
     * FS_LoadOverlay calls it (src/platform_pc/nitro/fs_nitro_pc.c). */
    void pc_register_static_init(int overlay, void (*fn)(void));
    static void NitroStaticInit(void);
    __attribute__((constructor)) static void pc_sinit_register_(void)
    {
        pc_register_static_init(PC_OVERLAY_INDEX, NitroStaticInit);
    }
#elif defined(SDK_CW) || defined(__MWERKS__)
    static void NitroStaticInit(void);

    #pragma define_section SINIT ".sinit" abs32 RWX
    #pragma section        SINIT begin

    SDK_FORCE_EXPORT static void * NitroStaticInit_[] = { NitroStaticInit };
    #pragma section        SINIT end
#elif defined(SDK_ADS)
    TO BE DEFINED
#elif defined(SDK_GCC)
    TO BE DEFINED
#endif
