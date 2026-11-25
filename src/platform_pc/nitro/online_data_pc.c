/*
 * online_data_pc.c - placeholders for data of the online-only libraries
 * (Nintendo Wi-Fi Connection root certificates, Wi-Fi Plaza tables) whose
 * assembly could not be converted. They are only referenced by address from
 * code that never runs offline. No game header is included on purpose.
 */
#define PLACEHOLDER(name) __attribute__((aligned(8))) unsigned char name[64]

PLACEHOLDER(gDwcCaBaltimoreCyberTrustRoot);
PLACEHOLDER(gDwcCaGlobalSignRoot);
PLACEHOLDER(gDwcCaGlobalSignRootR2);
PLACEHOLDER(gDwcCaGteCyberTrustGlobalRoot);
PLACEHOLDER(gDwcCaGteCyberTrustRoot);
PLACEHOLDER(gDwcCaNintendo);
PLACEHOLDER(gDwcCaRsaSecureServer);
PLACEHOLDER(gDwcCaThawtePremiumServer);
PLACEHOLDER(gDwcCaThawteServer);
PLACEHOLDER(gDwcCaVeriSignClass3);
PLACEHOLDER(gDwcCaVeriSignClass3G2);
PLACEHOLDER(gDwcCaVeriSignClass3G3);
PLACEHOLDER(gNdCaNintendo);
PLACEHOLDER(ov45_02255200);
PLACEHOLDER(ov45_02255358);

/* C++ runtime entry points referenced (by mangled name) from the Wi-Fi Plaza
 * lobby code, which is C on this decomp but calls MSL C++ helpers. */
#include <stdlib.h>

#define PC_STR2(x) #x
#define PC_STR(x) PC_STR2(x)
#define PC_LABEL(sym) __asm__(PC_STR(__USER_LABEL_PREFIX__) #sym)

const unsigned char pc_std_nothrow PC_LABEL(_ZSt7nothrow) = 0;             /* std::nothrow */

void *pc_new_array_nothrow(size_t size, const void *nt) PC_LABEL(_ZnamRKSt9nothrow_t);
void *pc_new_array_nothrow(size_t size, const void *nt) {                  /* operator new[](size_t, nothrow_t const&) */
    (void)nt;
    return malloc(size ? size : 1);
}

void pc_delete_array(void *p) PC_LABEL(_ZdaPv);
void pc_delete_array(void *p) {                                            /* operator delete[](void *) */
    free(p);
}

void pc_vector_throw_length_error(void) PC_LABEL(_ZNSt15__vector_commonILb1EE18throw_length_errorEv);
void pc_vector_throw_length_error(void) {
    abort();                                                               /* MSL: vector too long */
}
