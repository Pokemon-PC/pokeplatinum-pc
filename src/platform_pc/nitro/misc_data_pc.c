/*
 * misc_data_pc.c - data the DS build defines through linker tricks that do
 * not carry over to PC.
 *
 * Pokewalker IR: pokewalker_ir_requests.alias-map.json makes these symbols
 * aliases *inside* other .bss objects of that file. The PC build gives them
 * their own storage, with their declared types. The feature itself needs
 * the cartridge's infrared chip and is not functional on PC.
 */
#include "global.h"

#include "application/pokewalker/pokewalker_ir_internal.h"

u8 *gPokewalkerIrImageSizes[2];
u8 *gPokewalkerIrStrollSizes[2];
OSMessageQueue gPokewalkerIrMessageQueue;
PokewalkerIrTransfer gPokewalkerIrTransfer;
u8 gPokewalkerIrSendBuffer[0x80];
