/*
    Copyright 2016-2017 StapleButter

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include "ARM.h"


#define CARRY_ADD(a, b)  ((0xFFFFFFFF-a) < b)
#define CARRY_SUB(a, b)  (a >= b)

#define OVERFLOW_ADD(a, b, res)  ((!(((a) ^ (b)) & 0x80000000)) && (((a) ^ (res)) & 0x80000000))
#define OVERFLOW_SUB(a, b, res)  ((((a) ^ (b)) & 0x80000000) && (((a) ^ (res)) & 0x80000000))


namespace ARMInterpreter
{


#define LSL_IMM(x, s) \
    x <<= s;

#define LSR_IMM(x, s) \
    if (s == 0) x = 0; \
    else        x >>= s;

#define ASR_IMM(x, s) \
    if (s == 0) x = ((s32)x) >> 31; \
    else        x = ((s32)x) >> s;

#define ROR_IMM(x, s) \
    if (s == 0) \
    { \
        x = (x >> 1) | ((cpu->CPSR & 0x20000000) << 2); \
    } \
    else \
    { \
        x = ROR(x, s); \
    }

#define LSL_IMM_S(x, s) \
    if (s > 0) \
    { \
        cpu->SetC(x & (1<<(32-s))); \
        x <<= s; \
    }

#define LSR_IMM_S(x, s) \
    if (s == 0) { \
        cpu->SetC(x & (1<<31)); \
        x = 0; \
    } else { \
        cpu->SetC(x & (1<<(s-1))); \
        x >>= s; \
    }

#define ASR_IMM_S(x, s) \
    if (s == 0) { \
        cpu->SetC(x & (1<<31)); \
        x = ((s32)x) >> 31; \
    } else { \
        cpu->SetC(x & (1<<(s-1))); \
        x = ((s32)x) >> s; \
    }

#define ROR_IMM_S(x, s) \
    if (s == 0) \
    { \
        u32 newc = (x & 1); \
        x = (x >> 1) | ((cpu->CPSR & 0x20000000) << 2); \
        cpu->SetC(newc); \
    } \
    else \
    { \
        cpu->SetC(x & (1<<(s-1))); \
        x = ROR(x, s); \
    }

#define LSL_REG(x, s) \
    if (s > 31) x = 0; \
    else        x <<= s;

#define LSR_REG(x, s) \
    if (s > 31) x = 0; \
    else        x >>= s;

#define ASR_REG(x, s) \
    if (s > 31) x = ((s32)x) >> 31; \
    else        x = ((s32)x) >> s;

#define ROR_REG(x, s) \
    x = ROR(x, (s&0x1F));

#define LSL_REG_S(x, s) \
    if (s > 31)     { cpu->SetC(x & (1<<0));      x = 0; } \
    else if (s > 0) { cpu->SetC(x & (1<<(32-s))); x <<= s; }

#define LSR_REG_S(x, s) \
    if (s > 31)     { cpu->SetC(x & (1<<31));    x = 0; } \
    else if (s > 0) { cpu->SetC(x & (1<<(s-1))); x >>= s; }

#define ASR_REG_S(x, s) \
    if (s > 31)     { cpu->SetC(x & (1<<31));    x = ((s32)x) >> 31; } \
    else if (s > 0) { cpu->SetC(x & (1<<(s-1))); x = ((s32)x) >> s; }

#define ROR_REG_S(x, s) \
    if (s > 0) cpu->SetC(x & (1<<(s-1))); \
    x = ROR(x, (s&0x1F));



#define A_CALC_OP2_IMM \
    u32 b = ROR(cpu->CurInstr&0xFF, (cpu->CurInstr>>7)&0x1E);

#define A_CALC_OP2_REG_SHIFT_IMM(shiftop) \
    u32 b = cpu->R[cpu->CurInstr&0xF]; \
    u32 s = (cpu->CurInstr>>7)&0x1F; \
    shiftop(b, s);

#define A_CALC_OP2_REG_SHIFT_REG(shiftop) \
    u32 b = cpu->R[cpu->CurInstr&0xF]; \
    if ((cpu->CurInstr&0xF)==15) b += 4; \
    shiftop(b, cpu->R[(cpu->CurInstr>>8)&0xF]);


#define A_IMPLEMENT_ALU_OP(x,s) \
\
s32 A_##x##_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_IMM \
    A_##x(0) \
} \
s32 A_##x##_REG_LSL_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(LSL_IMM) \
    A_##x(0) \
} \
s32 A_##x##_REG_LSR_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(LSR_IMM) \
    A_##x(0) \
} \
s32 A_##x##_REG_ASR_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(ASR_IMM) \
    A_##x(0) \
} \
s32 A_##x##_REG_ROR_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(ROR_IMM) \
    A_##x(0) \
} \
s32 A_##x##_REG_LSL_REG(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(LSL_REG) \
    A_##x(1) \
} \
s32 A_##x##_REG_LSR_REG(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(LSR_REG) \
    A_##x(1) \
} \
s32 A_##x##_REG_ASR_REG(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(ASR_REG) \
    A_##x(1) \
} \
s32 A_##x##_REG_ROR_REG(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(ROR_REG) \
    A_##x(1) \
} \
s32 A_##x##_IMM_S(ARM* cpu) \
{ \
    A_CALC_OP2_IMM \
    A_##x##_S(0) \
} \
s32 A_##x##_REG_LSL_IMM_S(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(LSL_IMM##s) \
    A_##x##_S(0) \
} \
s32 A_##x##_REG_LSR_IMM_S(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(LSR_IMM##s) \
    A_##x##_S(0) \
} \
s32 A_##x##_REG_ASR_IMM_S(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(ASR_IMM##s) \
    A_##x##_S(0) \
} \
s32 A_##x##_REG_ROR_IMM_S(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(ROR_IMM##s) \
    A_##x##_S(0) \
} \
s32 A_##x##_REG_LSL_REG_S(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(LSL_REG##s) \
    A_##x##_S(1) \
} \
s32 A_##x##_REG_LSR_REG_S(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(LSR_REG##s) \
    A_##x##_S(1) \
} \
s32 A_##x##_REG_ASR_REG_S(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(ASR_REG##s) \
    A_##x##_S(1) \
} \
s32 A_##x##_REG_ROR_REG_S(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(ROR_REG##s) \
    A_##x##_S(1) \
}

#define A_IMPLEMENT_ALU_TEST(x,s) \
\
s32 A_##x##_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_IMM \
    A_##x(0) \
} \
s32 A_##x##_REG_LSL_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(LSL_IMM##s) \
    A_##x(0) \
} \
s32 A_##x##_REG_LSR_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(LSR_IMM##s) \
    A_##x(0) \
} \
s32 A_##x##_REG_ASR_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(ASR_IMM##s) \
    A_##x(0) \
} \
s32 A_##x##_REG_ROR_IMM(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_IMM(ROR_IMM##s) \
    A_##x(0) \
} \
s32 A_##x##_REG_LSL_REG(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(LSL_REG##s) \
    A_##x(1) \
} \
s32 A_##x##_REG_LSR_REG(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(LSR_REG##s) \
    A_##x(1) \
} \
s32 A_##x##_REG_ASR_REG(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(ASR_REG##s) \
    A_##x(1) \
} \
s32 A_##x##_REG_ROR_REG(ARM* cpu) \
{ \
    A_CALC_OP2_REG_SHIFT_REG(ROR_REG##s) \
    A_##x(1) \
}


#define A_AND(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a & b; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_AND_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a & b; \
    cpu->SetNZ(res & 0x80000000, \
               !res); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(AND,_S)


#define A_EOR(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a ^ b; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_EOR_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a ^ b; \
    cpu->SetNZ(res & 0x80000000, \
               !res); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(EOR,_S)


#define A_SUB(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a - b; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_SUB_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a - b; \
    cpu->SetNZCV(res & 0x80000000, \
                 !res, \
                 CARRY_SUB(a, b), \
                 OVERFLOW_SUB(a, b, res)); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(SUB,)


#define A_RSB(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = b - a; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_RSB_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = b - a; \
    cpu->SetNZCV(res & 0x80000000, \
                 !res, \
                 CARRY_SUB(b, a), \
                 OVERFLOW_SUB(b, a, res)); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(RSB,)


#define A_ADD(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a + b; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_ADD_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a + b; \
    cpu->SetNZCV(res & 0x80000000, \
                 !res, \
                 CARRY_ADD(a, b), \
                 OVERFLOW_ADD(a, b, res)); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(ADD,)


#define A_ADC(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a + b + (cpu->CPSR&0x20000000 ? 1:0); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_ADC_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res_tmp = a + b; \
    u32 carry = (cpu->CPSR&0x20000000 ? 1:0); \
    u32 res = res_tmp + carry; \
    cpu->SetNZCV(res & 0x80000000, \
                 !res, \
                 CARRY_ADD(a, b) | CARRY_ADD(res_tmp, carry), \
                 OVERFLOW_ADD(a, b, res_tmp) | OVERFLOW_ADD(res_tmp, carry, res)); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(ADC,)


#define A_SBC(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a - b - (cpu->CPSR&0x20000000 ? 0:1); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_SBC_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res_tmp = a - b; \
    u32 carry = (cpu->CPSR&0x20000000 ? 0:1); \
    u32 res = res_tmp - carry; \
    cpu->SetNZCV(res & 0x80000000, \
                 !res, \
                 CARRY_SUB(a, b) & CARRY_SUB(res_tmp, carry), \
                 OVERFLOW_SUB(a, b, res_tmp) | OVERFLOW_SUB(res_tmp, carry, res)); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(SBC,)


#define A_RSC(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = b - a - (cpu->CPSR&0x20000000 ? 0:1); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_RSC_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res_tmp = b - a; \
    u32 carry = (cpu->CPSR&0x20000000 ? 0:1); \
    u32 res = res_tmp - carry; \
    cpu->SetNZCV(res & 0x80000000, \
                 !res, \
                 CARRY_SUB(b, a) & CARRY_SUB(res_tmp, carry), \
                 OVERFLOW_SUB(b, a, res_tmp) | OVERFLOW_SUB(res_tmp, carry, res)); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(RSC,)


#define A_TST(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a & b; \
    cpu->SetNZ(res & 0x80000000, \
               !res); \
    return C_S(1) + C_I(c);

A_IMPLEMENT_ALU_TEST(TST,_S)


#define A_TEQ(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a ^ b; \
    cpu->SetNZ(res & 0x80000000, \
               !res); \
    return C_S(1) + C_I(c);

A_IMPLEMENT_ALU_TEST(TEQ,_S)


#define A_CMP(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a - b; \
    cpu->SetNZCV(res & 0x80000000, \
                 !res, \
                 CARRY_SUB(a, b), \
                 OVERFLOW_SUB(a, b, res)); \
    return C_S(1) + C_I(c);

A_IMPLEMENT_ALU_TEST(CMP,)


#define A_CMN(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a + b; \
    cpu->SetNZCV(res & 0x80000000, \
                 !res, \
                 CARRY_ADD(a, b), \
                 OVERFLOW_ADD(a, b, res)); \
    return C_S(1) + C_I(c);

A_IMPLEMENT_ALU_TEST(CMN,)


#define A_ORR(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a | b; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_ORR_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a | b; \
    cpu->SetNZ(res & 0x80000000, \
               !res); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(ORR,_S)


#define A_MOV(c) \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(b); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = b; \
        return C_S(1) + C_I(c); \
    }

#define A_MOV_S(c) \
    cpu->SetNZ(b & 0x80000000, \
               !b); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(b, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = b; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(MOV,_S)


#define A_BIC(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a & ~b; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

#define A_BIC_S(c) \
    u32 a = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 res = a & ~b; \
    cpu->SetNZ(res & 0x80000000, \
               !res); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(res, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = res; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(BIC,_S)


#define A_MVN(c) \
    b = ~b; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(b); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = b; \
        return C_S(1) + C_I(c); \
    }

#define A_MVN_S(c) \
    b = ~b; \
    cpu->SetNZ(b & 0x80000000, \
               !b); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        cpu->JumpTo(b, true); \
        return C_S(2) + C_I(c) + C_N(1); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = b; \
        return C_S(1) + C_I(c); \
    }

A_IMPLEMENT_ALU_OP(MVN,_S)



s32 A_MUL(ARM* cpu)
{
    u32 rm = cpu->R[cpu->CurInstr & 0xF];
    u32 rs = cpu->R[(cpu->CurInstr >> 8) & 0xF];

    u32 res = rm * rs;

    cpu->R[(cpu->CurInstr >> 16) & 0xF] = res;
    if (cpu->CurInstr & (1<<20))
    {
        cpu->SetNZ(res & 0x80000000,
                   !res);
        if (cpu->Num==1) cpu->SetC(0);
    }

    u32 cycles;
    if      ((rs & 0xFFFFFF00) == 0x00000000 || (rs & 0xFFFFFF00) == 0xFFFFFF00) cycles = 1;
    else if ((rs & 0xFFFF0000) == 0x00000000 || (rs & 0xFFFF0000) == 0xFFFF0000) cycles = 2;
    else if ((rs & 0xFF000000) == 0x00000000 || (rs & 0xFF000000) == 0xFF000000) cycles = 3;
    else cycles = 4;

    return C_S(1) + C_I(cycles);
}

s32 A_MLA(ARM* cpu)
{
    u32 rm = cpu->R[cpu->CurInstr & 0xF];
    u32 rs = cpu->R[(cpu->CurInstr >> 8) & 0xF];
    u32 rn = cpu->R[(cpu->CurInstr >> 12) & 0xF];

    u32 res = (rm * rs) + rn;

    cpu->R[(cpu->CurInstr >> 16) & 0xF] = res;
    if (cpu->CurInstr & (1<<20))
    {
        cpu->SetNZ(res & 0x80000000,
                   !res);
        if (cpu->Num==1) cpu->SetC(0);
    }

    u32 cycles;
    if      ((rs & 0xFFFFFF00) == 0x00000000 || (rs & 0xFFFFFF00) == 0xFFFFFF00) cycles = 2;
    else if ((rs & 0xFFFF0000) == 0x00000000 || (rs & 0xFFFF0000) == 0xFFFF0000) cycles = 3;
    else if ((rs & 0xFF000000) == 0x00000000 || (rs & 0xFF000000) == 0xFF000000) cycles = 4;
    else cycles = 5;

    return C_S(1) + C_I(cycles);
}

s32 A_UMULL(ARM* cpu)
{
    u32 rm = cpu->R[cpu->CurInstr & 0xF];
    u32 rs = cpu->R[(cpu->CurInstr >> 8) & 0xF];

    u64 res = (u64)rm * (u64)rs;

    cpu->R[(cpu->CurInstr >> 12) & 0xF] = (u32)res;
    cpu->R[(cpu->CurInstr >> 16) & 0xF] = (u32)(res >> 32ULL);
    if (cpu->CurInstr & (1<<20))
    {
        cpu->SetNZ((u32)(res >> 63ULL),
                   !res);
        if (cpu->Num==1) cpu->SetC(0);
    }

    u32 cycles;
    if      ((rs & 0xFFFFFF00) == 0x00000000) cycles = 2;
    else if ((rs & 0xFFFF0000) == 0x00000000) cycles = 3;
    else if ((rs & 0xFF000000) == 0x00000000) cycles = 4;
    else cycles = 5;

    return C_S(1) + C_I(cycles);
}

s32 A_UMLAL(ARM* cpu)
{
    u32 rm = cpu->R[cpu->CurInstr & 0xF];
    u32 rs = cpu->R[(cpu->CurInstr >> 8) & 0xF];

    u64 res = (u64)rm * (u64)rs;

    u64 rd = (u64)cpu->R[(cpu->CurInstr >> 12) & 0xF] | ((u64)cpu->R[(cpu->CurInstr >> 16) & 0xF] << 32ULL);
    res += rd;

    cpu->R[(cpu->CurInstr >> 12) & 0xF] = (u32)res;
    cpu->R[(cpu->CurInstr >> 16) & 0xF] = (u32)(res >> 32ULL);
    if (cpu->CurInstr & (1<<20))
    {
        cpu->SetNZ((u32)(res >> 63ULL),
                   !res);
        if (cpu->Num==1) cpu->SetC(0);
    }

    u32 cycles;
    if      ((rs & 0xFFFFFF00) == 0x00000000) cycles = 2;
    else if ((rs & 0xFFFF0000) == 0x00000000) cycles = 3;
    else if ((rs & 0xFF000000) == 0x00000000) cycles = 4;
    else cycles = 5;

    return C_S(1) + C_I(cycles);
}

s32 A_SMULL(ARM* cpu)
{
    u32 rm = cpu->R[cpu->CurInstr & 0xF];
    u32 rs = cpu->R[(cpu->CurInstr >> 8) & 0xF];

    s64 res = (s64)(s32)rm * (s64)(s32)rs;

    cpu->R[(cpu->CurInstr >> 12) & 0xF] = (u32)res;
    cpu->R[(cpu->CurInstr >> 16) & 0xF] = (u32)(res >> 32ULL);
    if (cpu->CurInstr & (1<<20))
    {
        cpu->SetNZ((u32)(res >> 63ULL),
                   !res);
        if (cpu->Num==1) cpu->SetC(0);
    }

    u32 cycles;
    if      ((rs & 0xFFFFFF00) == 0x00000000 || (rs & 0xFFFFFF00) == 0xFFFFFF00) cycles = 2;
    else if ((rs & 0xFFFF0000) == 0x00000000 || (rs & 0xFFFF0000) == 0xFFFF0000) cycles = 3;
    else if ((rs & 0xFF000000) == 0x00000000 || (rs & 0xFF000000) == 0xFF000000) cycles = 4;
    else cycles = 5;

    return C_S(1) + C_I(cycles);
}

s32 A_SMLAL(ARM* cpu)
{
    u32 rm = cpu->R[cpu->CurInstr & 0xF];
    u32 rs = cpu->R[(cpu->CurInstr >> 8) & 0xF];

    s64 res = (s64)(s32)rm * (s64)(s32)rs;

    s64 rd = (s64)((u64)cpu->R[(cpu->CurInstr >> 12) & 0xF] | ((u64)cpu->R[(cpu->CurInstr >> 16) & 0xF] << 32ULL));
    res += rd;

    cpu->R[(cpu->CurInstr >> 12) & 0xF] = (u32)res;
    cpu->R[(cpu->CurInstr >> 16) & 0xF] = (u32)(res >> 32ULL);
    if (cpu->CurInstr & (1<<20))
    {
        cpu->SetNZ((u32)(res >> 63ULL),
                   !res);
        if (cpu->Num==1) cpu->SetC(0);
    }

    u32 cycles;
    if      ((rs & 0xFFFFFF00) == 0x00000000 || (rs & 0xFFFFFF00) == 0xFFFFFF00) cycles = 2;
    else if ((rs & 0xFFFF0000) == 0x00000000 || (rs & 0xFFFF0000) == 0xFFFF0000) cycles = 3;
    else if ((rs & 0xFF000000) == 0x00000000 || (rs & 0xFF000000) == 0xFF000000) cycles = 4;
    else cycles = 5;

    return C_S(1) + C_I(cycles);
}



s32 A_CLZ(ARM* cpu)
{
    // TODO: ARM9 only

    u32 val = cpu->R[cpu->CurInstr & 0xF];

    u32 res = 0;
    while ((val & 0xFF000000) == 0)
    {
        res += 8;
        val <<= 8;
        val |= 0xFF;
    }
    while ((val & 0x80000000) == 0)
    {
        res++;
        val <<= 1;
        val |= 0x1;
    }

    cpu->R[(cpu->CurInstr >> 12) & 0xF] = res;

    return C_S(1);
}



// ---- THUMB ----------------------------------



s32 T_LSL_IMM(ARM* cpu)
{
    u32 op = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 s = (cpu->CurInstr >> 6) & 0x1F;
    LSL_IMM_S(op, s);
    cpu->R[cpu->CurInstr & 0x7] = op;
    cpu->SetNZ(op & 0x80000000,
               !op);
    return C_S(1);
}

s32 T_LSR_IMM(ARM* cpu)
{
    u32 op = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 s = (cpu->CurInstr >> 6) & 0x1F;
    LSR_IMM_S(op, s);
    cpu->R[cpu->CurInstr & 0x7] = op;
    cpu->SetNZ(op & 0x80000000,
               !op);
    return C_S(1);
}

s32 T_ASR_IMM(ARM* cpu)
{
    u32 op = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 s = (cpu->CurInstr >> 6) & 0x1F;
    ASR_IMM_S(op, s);
    cpu->R[cpu->CurInstr & 0x7] = op;
    cpu->SetNZ(op & 0x80000000,
               !op);
    return C_S(1);
}

s32 T_ADD_REG_(ARM* cpu)
{
    u32 a = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 6) & 0x7];
    u32 res = a + b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_ADD(a, b),
                 OVERFLOW_ADD(a, b, res));
    return C_S(1);
}

s32 T_SUB_REG_(ARM* cpu)
{
    u32 a = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 6) & 0x7];
    u32 res = a - b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_SUB(a, b),
                 OVERFLOW_SUB(a, b, res));
    return C_S(1);
}

s32 T_ADD_IMM_(ARM* cpu)
{
    u32 a = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 b = (cpu->CurInstr >> 6) & 0x7;
    u32 res = a + b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_ADD(a, b),
                 OVERFLOW_ADD(a, b, res));
    return C_S(1);
}

s32 T_SUB_IMM_(ARM* cpu)
{
    u32 a = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 b = (cpu->CurInstr >> 6) & 0x7;
    u32 res = a - b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_SUB(a, b),
                 OVERFLOW_SUB(a, b, res));
    return C_S(1);
}

s32 T_MOV_IMM(ARM* cpu)
{
    u32 b = cpu->CurInstr & 0xFF;
    cpu->R[(cpu->CurInstr >> 8) & 0x7] = b;
    cpu->SetNZ(0,
               !b);
    return C_S(1);
}

s32 T_CMP_IMM(ARM* cpu)
{
    u32 a = cpu->R[(cpu->CurInstr >> 8) & 0x7];
    u32 b = cpu->CurInstr & 0xFF;
    u32 res = a - b;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_SUB(a, b),
                 OVERFLOW_SUB(a, b, res));
    return C_S(1);
}

s32 T_ADD_IMM(ARM* cpu)
{
    u32 a = cpu->R[(cpu->CurInstr >> 8) & 0x7];
    u32 b = cpu->CurInstr & 0xFF;
    u32 res = a + b;
    cpu->R[(cpu->CurInstr >> 8) & 0x7] = res;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_ADD(a, b),
                 OVERFLOW_ADD(a, b, res));
    return C_S(1);
}

s32 T_SUB_IMM(ARM* cpu)
{
    u32 a = cpu->R[(cpu->CurInstr >> 8) & 0x7];
    u32 b = cpu->CurInstr & 0xFF;
    u32 res = a - b;
    cpu->R[(cpu->CurInstr >> 8) & 0x7] = res;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_SUB(a, b),
                 OVERFLOW_SUB(a, b, res));
    return C_S(1);
}


s32 T_AND_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = a & b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZ(res & 0x80000000,
               !res);
    return C_S(1);
}

s32 T_EOR_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = a ^ b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZ(res & 0x80000000,
               !res);
    return C_S(1);
}

s32 T_LSL_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7] & 0xFF;
    LSL_REG_S(a, b);
    cpu->R[cpu->CurInstr & 0x7] = a;
    cpu->SetNZ(a & 0x80000000,
               !a);
    return C_S(1) + C_I(1);
}

s32 T_LSR_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7] & 0xFF;
    LSR_REG_S(a, b);
    cpu->R[cpu->CurInstr & 0x7] = a;
    cpu->SetNZ(a & 0x80000000,
               !a);
    return C_S(1) + C_I(1);
}

s32 T_ASR_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7] & 0xFF;
    ASR_REG_S(a, b);
    cpu->R[cpu->CurInstr & 0x7] = a;
    cpu->SetNZ(a & 0x80000000,
               !a);
    return C_S(1) + C_I(1);
}

s32 T_ADC_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res_tmp = a + b;
    u32 carry = (cpu->CPSR&0x20000000 ? 1:0);
    u32 res = res_tmp + carry;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_ADD(a, b) | CARRY_ADD(res_tmp, carry),
                 OVERFLOW_ADD(a, b, res_tmp) | OVERFLOW_ADD(res_tmp, carry, res));
    return C_S(1);
}

s32 T_SBC_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res_tmp = a - b;
    u32 carry = (cpu->CPSR&0x20000000 ? 0:1);
    u32 res = res_tmp - carry;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_SUB(a, b) & CARRY_SUB(res_tmp, carry),
                 OVERFLOW_SUB(a, b, res_tmp) | OVERFLOW_SUB(res_tmp, carry, res));
    return C_S(1);
}

s32 T_ROR_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7] & 0xFF;
    ROR_REG_S(a, b);
    cpu->R[cpu->CurInstr & 0x7] = a;
    cpu->SetNZ(a & 0x80000000,
               !a);
    return C_S(1) + C_I(1);
}

s32 T_TST_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = a & b;
    cpu->SetNZ(res & 0x80000000,
               !res);
    return C_S(1);
}

s32 T_NEG_REG(ARM* cpu)
{
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = -b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZ(res & 0x80000000,
               !res);
    return C_S(1);
}

s32 T_CMP_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = a - b;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_SUB(a, b),
                 OVERFLOW_SUB(a, b, res));
    return C_S(1);
}

s32 T_CMN_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = a + b;
    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_ADD(a, b),
                 OVERFLOW_ADD(a, b, res));
    return C_S(1);
}

s32 T_ORR_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = a | b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZ(res & 0x80000000,
               !res);
    return C_S(1);
}

s32 T_MUL_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = a * b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZ(res & 0x80000000,
               !res);

    s32 cycles = C_S(1);
    if (cpu->Num == 0)
    {
        cycles += C_I(3);
    }
    else
    {
        cpu->SetC(0); // carry flag destroyed, they say. whatever that means...
        if      (a & 0xFF000000) cycles += C_I(4);
        else if (a & 0x00FF0000) cycles += C_I(3);
        else if (a & 0x0000FF00) cycles += C_I(2);
        else                     cycles += C_I(1);
    }
    return cycles;
}

s32 T_BIC_REG(ARM* cpu)
{
    u32 a = cpu->R[cpu->CurInstr & 0x7];
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = a & ~b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZ(res & 0x80000000,
               !res);
    return C_S(1);
}

s32 T_MVN_REG(ARM* cpu)
{
    u32 b = cpu->R[(cpu->CurInstr >> 3) & 0x7];
    u32 res = ~b;
    cpu->R[cpu->CurInstr & 0x7] = res;
    cpu->SetNZ(res & 0x80000000,
               !res);
    return C_S(1);
}


s32 T_ADD_HIREG(ARM* cpu)
{
    u32 rd = (cpu->CurInstr & 0x7) | ((cpu->CurInstr >> 4) & 0x8);
    u32 rs = (cpu->CurInstr >> 3) & 0xF;

    u32 a = cpu->R[rd];
    u32 b = cpu->R[rs];

    if (rd == 15)
    {
        cpu->JumpTo((a + b) | 1);
        return C_S(2) + C_N(1);
    }
    else
    {
        cpu->R[rd] = a + b;
        return C_S(1);
    }
}

s32 T_CMP_HIREG(ARM* cpu)
{
    u32 rd = (cpu->CurInstr & 0x7) | ((cpu->CurInstr >> 4) & 0x8);
    u32 rs = (cpu->CurInstr >> 3) & 0xF;

    u32 a = cpu->R[rd];
    u32 b = cpu->R[rs];
    u32 res = a - b;

    cpu->SetNZCV(res & 0x80000000,
                 !res,
                 CARRY_SUB(a, b),
                 OVERFLOW_SUB(a, b, res));
    return C_S(1);
}

s32 T_MOV_HIREG(ARM* cpu)
{
    u32 rd = (cpu->CurInstr & 0x7) | ((cpu->CurInstr >> 4) & 0x8);
    u32 rs = (cpu->CurInstr >> 3) & 0xF;

    if (rd == 15)
    {
        cpu->JumpTo(cpu->R[rs] | 1);
        return C_S(2) + C_N(1);
    }
    else
    {
        cpu->R[rd] = cpu->R[rs];
        return C_S(1);
    }
}


s32 T_ADD_PCREL(ARM* cpu)
{
    u32 val = cpu->R[15] & ~2;
    val += ((cpu->CurInstr & 0xFF) << 2);
    cpu->R[(cpu->CurInstr >> 8) & 0x7] = val;
    return C_S(1);
}

s32 T_ADD_SPREL(ARM* cpu)
{
    u32 val = cpu->R[13];
    val += ((cpu->CurInstr & 0xFF) << 2);
    cpu->R[(cpu->CurInstr >> 8) & 0x7] = val;
    return C_S(1);
}

s32 T_ADD_SP(ARM* cpu)
{
    u32 val = cpu->R[13];
    if (cpu->CurInstr & (1<<7))
        val -= ((cpu->CurInstr & 0x7F) << 2);
    else
        val += ((cpu->CurInstr & 0x7F) << 2);
    cpu->R[13] = val;
    return C_S(1);
}


}
