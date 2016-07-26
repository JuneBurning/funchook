/* -*- indent-tabs-mode: nil -*-
 *
 * This file is part of Duckhook.
 * https://github.com/kubo/duckhook
 *
 * Duckhook is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 2 of the License, or (at your
 * option) any later version.
 *
 * As a special exception, the copyright holders of this library give you
 * permission to link this library with independent modules to produce an
 * executable, regardless of the license terms of these independent
 * modules, and to copy and distribute the resulting executable under
 * terms of your choice, provided that you also meet, for each linked
 * independent module, the terms and conditions of the license of that
 * module. An independent module is a module which is not derived from or
 * based on this library. If you modify this library, you may extend this
 * exception to your version of the library, but you are not obliged to
 * do so. If you do not wish to do so, delete this exception statement
 * from your version.
 *
 * Duckhook is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Duckhook. If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <distorm.h>
#include <mnemonics.h>
#ifdef WIN32
#include <windows.h>
#endif
#include "duckhook_internal.h"

/* RIP-relative address information */
typedef struct {
    uint8_t *addr; /* absolute address */
    intptr_t raddr; /* relative address */
    int offset;
    int size;
} rip_relative_t;

typedef struct {
    duckhook_t *duckhook;
    rip_displacement_t *rip_disp;
    const uint8_t *src;
    const uint8_t *dst_base;
    uint8_t *dst;
} make_trampoline_context_t;

#define NOP_INSTRUCTION 0x90

#if defined(__linux) && defined(__i386)
static int handle_x86_get_pc_thunk(make_trampoline_context_t *ctx, const _DInst *di);
#else
#define handle_x86_get_pc_thunk(ctx, di) (0)
#endif
static void log_instruction(duckhook_t *duckhook, const _CodeInfo *ci, const _DInst *dis);
static void get_rip_relative(const make_trampoline_context_t *ctx, rip_relative_t *rel_disp, rip_relative_t *rel_imm, const _DInst *di);
static int handle_rip_relative(make_trampoline_context_t *ctx, const rip_relative_t *rel, const _DInst *di);

int duckhook_write_jump32(duckhook_t *duckhook, const uint8_t *src, const uint8_t *dst, uint8_t *out)
{
    out[0] = 0xe9;
    *(int*)(out + 1) = (int)(dst - (src + 5));
    duckhook_log(duckhook, "  Write jump32 0x0%"SIZE_T_WIDTH SIZE_T_FMT"x -> 0x0%"SIZE_T_WIDTH SIZE_T_FMT"x\n",
                 (size_t)src, (size_t)dst);
    return 0;
}

#ifdef CPU_X86_64

int duckhook_write_jump64(duckhook_t *duckhook, uint8_t *src, const uint8_t *dst)
{
    src[0] = 0xFF;
    src[1] = 0x25;
    src[2] = 0x00;
    src[3] = 0x00;
    src[4] = 0x00;
    src[5] = 0x00;
    *(const uint8_t**)(src + 6) = dst;
    duckhook_log(duckhook, "  Write jump64 0x0%"SIZE_T_WIDTH SIZE_T_FMT"x -> 0x0%"SIZE_T_WIDTH SIZE_T_FMT"x\n",
                 (size_t)src, (size_t)dst);
    return 0;
}

int duckhook_within_32bit_relative(const uint8_t *src, const uint8_t *dst)
{
    int64_t diff = (int64_t)(dst - src);
    return (INT32_MIN <= diff && diff <= INT32_MAX);
}

int duckhook_jump32_avail(const uint8_t *src, const uint8_t *dst)
{
    return duckhook_within_32bit_relative(src + 5, dst);
}

#endif

int duckhook_make_trampoline(duckhook_t *duckhook, rip_displacement_t *disp, const uint8_t *func, uint8_t *trampoline)
{
    make_trampoline_context_t ctx;
    _DInst dis[MAX_INSN_CHECK_SIZE];
    unsigned int di_cnt = 0;
    _CodeInfo ci;
    _DecodeResult decres;
    int rv;
    int i;

    memset(trampoline, NOP_INSTRUCTION, TRAMPOLINE_SIZE);
    ctx.duckhook = duckhook;
    ctx.rip_disp = disp;
    ctx.src = func;
    ctx.dst_base = ctx.dst = trampoline;

    ci.codeOffset = (_OffsetType)(size_t)func;
    ci.code = func;
    ci.codeLen = MAX_INSN_CHECK_SIZE;
#ifdef CPU_X86_64
    ci.dt = Decode64Bits;
#else
    ci.dt = Decode32Bits;
#endif
    ci.features = DF_STOP_ON_RET;
    decres = distorm_decompose64(&ci, dis, MAX_INSN_CHECK_SIZE, &di_cnt);
    if (decres != DECRES_SUCCESS) {
        duckhook_set_error_message(duckhook, "Disassemble Error: %d", decres);
        return DUCKHOOK_ERROR_DISASSEMBLY;
    }
    duckhook_log(duckhook, "  Original Instructions:\n");
    for (i = 0; i < di_cnt; i++) {
        const _DInst *di = &dis[i];
        rip_relative_t rel_disp;
        rip_relative_t rel_imm;

        log_instruction(duckhook, &ci, di);

        if (handle_x86_get_pc_thunk(&ctx, di)) {
            ;
        } else {
            memcpy(ctx.dst, ctx.src, di->size);
            get_rip_relative(&ctx, &rel_disp, &rel_imm, di);
            rv = handle_rip_relative(&ctx, &rel_disp, di);
            if (rv != 0) {
                return rv;
            }
            rv = handle_rip_relative(&ctx, &rel_imm, di);
            if (rv != 0) {
                return rv;
            }
            ctx.src += di->size;
            ctx.dst += di->size;
        }
        if (ctx.src - func >= JUMP32_SIZE) {
            ctx.dst[0] = 0xe9; /* unconditional jump */
            disp[0].dst_addr = ctx.src;
            disp[0].src_addr_offset = (ctx.dst - ctx.dst_base) + 5;
            disp[0].pos_offset = (ctx.dst - ctx.dst_base) + 1;
            while (++i < di_cnt) {
                const _DInst *di = &dis[i];
                log_instruction(duckhook, &ci, di);
                get_rip_relative(&ctx, &rel_disp, &rel_imm, di);
                if (func <= rel_imm.addr && rel_imm.addr < func + JUMP32_SIZE) {
                    /* jump to the hot-patched region. */
                    duckhook_set_error_message(duckhook, "instruction jumping back to the hot-patched region was found");
                    return DUCKHOOK_ERROR_FOUND_BACK_JUMP;
                }
            }
            return 0;
        }
    }
    /* too short function. Check whether NOP instructions continue. */
    while (ctx.src - func < JUMP32_SIZE) {
        if (*ctx.src != NOP_INSTRUCTION) {
            duckhook_set_error_message(duckhook, "Too short instructions");
            return DUCKHOOK_ERROR_TOO_SHORT_INSTRUCTIONS;
        }
        ctx.src++;
    }
    return 0;
}

void duckhook_log_trampoline(duckhook_t *duckhook, const uint8_t *trampoline)
{
    _DInst dis[TRAMPOLINE_SIZE];
    unsigned int di_cnt = 0;
    _CodeInfo ci;
    _DecodeResult decres;
    int i;

    if (duckhook_debug_file == NULL) {
        return;
    }

    duckhook_log(duckhook, "  Trampoline Instructions:\n");
    ci.codeOffset = (_OffsetType)(size_t)trampoline;
    ci.code = trampoline;
    ci.codeLen = TRAMPOLINE_SIZE;
#ifdef CPU_X86_64
    ci.dt = Decode64Bits;
#else
    ci.dt = Decode32Bits;
#endif
    ci.features = DF_NONE;
    decres = distorm_decompose64(&ci, dis, TRAMPOLINE_SIZE, &di_cnt);
    if (decres != DECRES_SUCCESS) {
        duckhook_log(duckhook, "  Failed to decode trampoline\n    ");
        for (i = 0; i < TRAMPOLINE_SIZE; i++) {
            duckhook_log(duckhook, " %02x", trampoline[i]);
        }
        duckhook_log(duckhook, "\n");
        return;
    }
    while (di_cnt > 0 && dis[di_cnt - 1].opcode == I_NOP) {
        di_cnt--;
    }
    for (i = 0; i < di_cnt; i++) {
        _DecodedInst dec;
        distorm_format64(&ci, &dis[i], &dec);
        duckhook_log(duckhook, "    %0"SIZE_T_WIDTH SIZE_T_FMT"x (%02d) %-24s %s%s%s\n",
                     (size_t)dec.offset, dec.size, (char*)dec.instructionHex.p,
                     (char*)dec.mnemonic.p, dec.operands.length != 0 ? " " : "", (char*)dec.operands.p);
    }
}

#ifndef handle_x86_get_pc_thunk
/* special cases to handle "call __x86.get_pc_thunk.??"
 * If the target instructions are "movl (%esp), %???; ret",
 * use "movl di->addr + 5, %???" instead.
 */
static int handle_x86_get_pc_thunk(make_trampoline_context_t *ctx, const _DInst *di)
{
    uint32_t eip = 0;
    const char *reg_name = NULL;

    if (*ctx->src == 0xe8) {
        uint32_t first_4_bytes = *(uint32_t*)(size_t)INSTRUCTION_GET_TARGET(di);

        eip = (uint32_t)(di->addr + 5);
        switch (first_4_bytes) {
        case 0xc324048b: /* 8b 04 24 c3: movl (%esp), %eax; ret */
            reg_name = "ax";
            *ctx->dst = 0xb8; /*         movl di->addr + 5, %eax */
            *(uint32_t*)(ctx->dst + 1) = eip;
            goto fixed;
        case 0xc3241c8b: /* 8b 1c 24 c3: movl (%esp), %ebx; ret */
            reg_name = "bx";
            *ctx->dst = 0xbb; /*         movl di->addr + 5, %ebx */
            *(uint32_t*)(ctx->dst + 1) = eip;
            goto fixed;
        case 0xc3240c8b: /* 8b 0c 24 c3: movl (%esp), %ecx; ret */
            reg_name = "cx";
            *ctx->dst = 0xb9; /*         movl di->addr + 5, %ecx */
            *(uint32_t*)(ctx->dst + 1) = eip;
            goto fixed;
        case 0xc324148b: /* 8b 14 24 c3: movl (%esp), %edx; ret */
            reg_name = "dx";
            *ctx->dst = 0xba; /*         movl di->addr + 5, %edx */
            *(uint32_t*)(ctx->dst + 1) = eip;
            goto fixed;
        case 0xc324348b: /* 8b 34 24 c3: movl (%esp), %esi; ret */
            reg_name = "si";
            *ctx->dst = 0xbe; /*         movl di->addr + 5, %esi */
            *(uint32_t*)(ctx->dst + 1) = eip;
            goto fixed;
        case 0xc3243c8b: /* 8b 3c 24 c3: movl (%esp), %edi; ret */
            reg_name = "di";
            *ctx->dst = 0xbf; /*         movl di->addr + 5, %edi */
            *(uint32_t*)(ctx->dst + 1) = eip;
            goto fixed;
        case 0xc3242c8b: /* 8b 2c 24 c3: movl (%esp), %ebp; ret */
            reg_name = "bp";
            *ctx->dst = 0xbd; /*         movl di->addr + 5, %ebp */
            *(uint32_t*)(ctx->dst + 1) = eip;
            goto fixed;
        }
    }
    return 0;

fixed:
    duckhook_log(ctx->duckhook, "      use 'MOV E%c%c, 0x%x' instead of 'CALL __x86.get_pc_thunk.%s'\n",
                 reg_name[0] + 'A' - 'a',
                 reg_name[1] + 'A' - 'a',
                 eip, reg_name);
    ctx->dst += 5;
    ctx->src += 5;
    return 1;
}
#endif

static void log_instruction(duckhook_t *duckhook, const _CodeInfo *ci, const _DInst *dis)
{
    _DecodedInst dec;
    distorm_format64(ci, dis, &dec);
    duckhook_log(duckhook, "    %0"SIZE_T_WIDTH SIZE_T_FMT"x (%02d) %-24s %s%s%s\n",
                 (size_t)dec.offset, dec.size, (char*)dec.instructionHex.p,
                 (char*)dec.mnemonic.p, dec.operands.length != 0 ? " " : "", (char*)dec.operands.p);
}

static void get_rip_relative(const make_trampoline_context_t *ctx, rip_relative_t *rel_disp, rip_relative_t *rel_imm, const _DInst *di)
{
    int opsiz = 0;
    int disp_offset = 0;
    int imm_offset = 0;
    int i;

    memset(rel_disp, 0, sizeof(rip_relative_t));
    memset(rel_imm, 0, sizeof(rip_relative_t));

    /*
     * Estimate total operand size and RIP-relative address offsets.
     */
    for (i = 0; i < OPERANDS_NO && di->ops[i].type != O_NONE; i++) {
        const _Operand *op = &di->ops[i];
        switch (op->type) {
        case O_IMM:
            opsiz += op->size / 8;
            break;
        case O_PC:
            rel_imm->addr = (uint8_t*)(size_t)(di->addr + di->size + di->imm.addr);
            rel_imm->raddr = di->imm.addr;
            rel_imm->size = op->size;
            imm_offset = opsiz;
            opsiz += op->size / 8;
            break;
        case O_SMEM:
            if (di->dispSize != 0 && op->index == R_RIP) {
                rel_disp->addr = (uint8_t*)(size_t)(di->addr + di->size + di->disp);
                rel_disp->raddr = di->disp;
                rel_disp->size = di->dispSize;
                disp_offset = opsiz;
            }
            opsiz += di->dispSize / 8;
            break;
        case O_MEM:
        case O_DISP:
            opsiz += di->dispSize / 8;
            break;
        }
    }
    switch (di->opcode) {
    /* CMPSD */
    case I_CMPEQSD:
    case I_CMPLTSD:
    case I_CMPLESD:
    case I_CMPUNORDSD:
    case I_CMPNEQSD:
    case I_CMPNLTSD:
    case I_CMPNLESD:
    case I_CMPORDSD:
    case I_VCMPEQSD:
    case I_VCMPLTSD:
    case I_VCMPLESD:
    case I_VCMPUNORDSD:
    case I_VCMPNEQSD:
    case I_VCMPNLTSD:
    case I_VCMPNLESD:
    case I_VCMPORDSD:
    case I_VCMPEQ_UQSD:
    case I_VCMPNGESD:
    case I_VCMPNGTSD:
    case I_VCMPFALSESD:
    case I_VCMPNEQ_OQSD:
    case I_VCMPGESD:
    case I_VCMPGTSD:
    case I_VCMPTRUESD:
    case I_VCMPEQ_OSSD:
    case I_VCMPLT_OQSD:
    case I_VCMPLE_OQSD:
    case I_VCMPUNORD_SSD:
    case I_VCMPNEQ_USSD:
    case I_VCMPNLT_UQSD:
    case I_VCMPNLE_UQSD:
    case I_VCMPORD_SSD:
    case I_VCMPEQ_USSD:
    case I_VCMPNGE_UQSD:
    case I_VCMPNGT_UQSD:
    case I_VCMPFALSE_OSSD:
    case I_VCMPNEQ_OSSD:
    case I_VCMPGE_OQSD:
    case I_VCMPGT_OQSD:
    /* CMPSS */
    case I_CMPEQSS:
    case I_CMPLTSS:
    case I_CMPLESS:
    case I_CMPUNORDSS:
    case I_CMPNEQSS:
    case I_CMPNLTSS:
    case I_CMPNLESS:
    case I_CMPORDSS:
    case I_VCMPEQSS:
    case I_VCMPLTSS:
    case I_VCMPLESS:
    case I_VCMPUNORDSS:
    case I_VCMPNEQSS:
    case I_VCMPNLTSS:
    case I_VCMPNLESS:
    case I_VCMPORDSS:
    case I_VCMPEQ_UQSS:
    case I_VCMPNGESS:
    case I_VCMPNGTSS:
    case I_VCMPFALSESS:
    case I_VCMPNEQ_OQSS:
    case I_VCMPGESS:
    case I_VCMPGTSS:
    case I_VCMPTRUESS:
    case I_VCMPEQ_OSSS:
    case I_VCMPLT_OQSS:
    case I_VCMPLE_OQSS:
    case I_VCMPUNORD_SSS:
    case I_VCMPNEQ_USSS:
    case I_VCMPNLT_UQSS:
    case I_VCMPNLE_UQSS:
    case I_VCMPORD_SSS:
    case I_VCMPEQ_USSS:
    case I_VCMPNGE_UQSS:
    case I_VCMPNGT_UQSS:
    case I_VCMPFALSE_OSSS:
    case I_VCMPNEQ_OSSS:
    case I_VCMPGE_OQSS:
    case I_VCMPGT_OQSS:
    /* CMPPD */
    case I_CMPEQPD:
    case I_CMPLTPD:
    case I_CMPLEPD:
    case I_CMPUNORDPD:
    case I_CMPNEQPD:
    case I_CMPNLTPD:
    case I_CMPNLEPD:
    case I_CMPORDPD:
    case I_VCMPEQPD:
    case I_VCMPLTPD:
    case I_VCMPLEPD:
    case I_VCMPUNORDPD:
    case I_VCMPNEQPD:
    case I_VCMPNLTPD:
    case I_VCMPNLEPD:
    case I_VCMPORDPD:
    case I_VCMPEQ_UQPD:
    case I_VCMPNGEPD:
    case I_VCMPNGTPD:
    case I_VCMPFALSEPD:
    case I_VCMPNEQ_OQPD:
    case I_VCMPGEPD:
    case I_VCMPGTPD:
    case I_VCMPTRUEPD:
    case I_VCMPEQ_OSPD:
    case I_VCMPLT_OQPD:
    case I_VCMPLE_OQPD:
    case I_VCMPUNORD_SPD:
    case I_VCMPNEQ_USPD:
    case I_VCMPNLT_UQPD:
    case I_VCMPNLE_UQPD:
    case I_VCMPORD_SPD:
    case I_VCMPEQ_USPD:
    case I_VCMPNGE_UQPD:
    case I_VCMPNGT_UQPD:
    case I_VCMPFALSE_OSPD:
    case I_VCMPNEQ_OSPD:
    case I_VCMPGE_OQPD:
    case I_VCMPGT_OQPD:
    case I_VCMPTRUE_USPD:
    /* CMPPS */
    case I_CMPEQPS:
    case I_CMPLTPS:
    case I_CMPLEPS:
    case I_CMPUNORDPS:
    case I_CMPNEQPS:
    case I_CMPNLTPS:
    case I_CMPNLEPS:
    case I_CMPORDPS:
    case I_VCMPEQPS:
    case I_VCMPLTPS:
    case I_VCMPLEPS:
    case I_VCMPUNORDPS:
    case I_VCMPNEQPS:
    case I_VCMPNLTPS:
    case I_VCMPNLEPS:
    case I_VCMPORDPS:
    case I_VCMPEQ_UQPS:
    case I_VCMPNGEPS:
    case I_VCMPNGTPS:
    case I_VCMPFALSEPS:
    case I_VCMPNEQ_OQPS:
    case I_VCMPGEPS:
    case I_VCMPGTPS:
    case I_VCMPTRUEPS:
    case I_VCMPEQ_OSPS:
    case I_VCMPLT_OQPS:
    case I_VCMPLE_OQPS:
    case I_VCMPUNORD_SPS:
    case I_VCMPNEQ_USPS:
    case I_VCMPNLT_UQPS:
    case I_VCMPNLE_UQPS:
    case I_VCMPORD_SPS:
    case I_VCMPEQ_USPS:
    case I_VCMPNGE_UQPS:
    case I_VCMPNGT_UQPS:
    case I_VCMPFALSE_OSPS:
    case I_VCMPNEQ_OSPS:
    case I_VCMPGE_OQPS:
    case I_VCMPGT_OQPS:
    case I_VCMPTRUE_USPS:
    /* ohters */
    case I_PI2FD:
    case I_PI2FW:
    case I_PF2IW:
    case I_PF2ID:
    case I_PSWAPD:
    case I_VPBLENDVB:
    case I_PFNACC:
        opsiz++;
    }

    if (rel_disp->size > 0) {
        rel_disp->offset = di->size - opsiz + disp_offset;
        duckhook_log(ctx->duckhook, "      ip-relative %08x, absolute address= %0"SIZE_T_WIDTH SIZE_T_FMT"x, offset=%d, size=%d\n",
                     (uint32_t)rel_disp->raddr, (size_t)rel_disp->addr, rel_disp->offset, rel_disp->size);
    }
    if (rel_imm->size > 0) {
        rel_imm->offset = di->size - opsiz + imm_offset;
        duckhook_log(ctx->duckhook, "      ip-relative %08x, absolute address= %0"SIZE_T_WIDTH SIZE_T_FMT"x, offset=%d, size=%d\n",
                     (uint32_t)rel_imm->raddr, (size_t)rel_imm->addr, rel_imm->offset, rel_imm->size);
    }
}

/*
 * Fix RIP-relative address in an instruction
 */
static int handle_rip_relative(make_trampoline_context_t *ctx, const rip_relative_t *rel, const _DInst *di)
{
    if (rel->size == 32) {
        if (*(int32_t*)(ctx->dst + rel->offset) != (uint32_t)rel->raddr) {
            /* sanity check.
             * reach here if opsiz and/or disp_offset are incorrectly
             * estimated.
             */
            duckhook_set_error_message(ctx->duckhook, "Invalid ip-relative offset %d. The value at the offset should be %08x but %08x",
                         rel->offset, (uint32_t)rel->raddr, *(int32_t*)(ctx->dst + rel->offset));
            return DUCKHOOK_ERROR_IP_RELATIVE_OFFSET;
        }
        ctx->rip_disp[1].dst_addr = rel->addr;
        ctx->rip_disp[1].src_addr_offset = (ctx->dst - ctx->dst_base) + di->size;;
        ctx->rip_disp[1].pos_offset = (ctx->dst - ctx->dst_base) + rel->offset;
    } else if (rel->size != 0) {
        duckhook_set_error_message(ctx->duckhook, "Could not fix ip-relative address. The size is not 32.");
        return DUCKHOOK_ERROR_CANNOT_FIX_IP_RELATIVE;
    }
    return 0;
}
