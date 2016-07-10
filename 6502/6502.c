#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

/*
	TO DO:
		- addressing modes, page boundary crossing (machine cycles + 1 in some cases)
		- Rewrite the statusregister struct to something easier
		- define the constant addresses for the ROMS and memory banks
		- make macros from the mnemnonic functions

*/
typedef struct StatusRegisters {
	uint8_t C:1;  // Carry           1=true
	uint8_t Z:1;  // Zero Result     1=result zero
	uint8_t I:1;  // IRQ Disable     1=Disable
	uint8_t D:1;  // Decimal mode    1=true
	uint8_t B:1;  // BRK Command
	uint8_t dc:1; // Expansion
	uint8_t V:1;  // Overflow        1=true
	uint8_t N:1;  // Negative Result 1=Negative
} StatusRegisters;

typedef struct State6510 {
	uint8_t  A;  // Accumulator A
	uint8_t  X;  // Index Register X
	uint8_t  Y;  // Index Register Y
	uint16_t PC; // Program Counter Points to the current address
	uint16_t SP; // Stack Pointer points to the next available location in the stack.
	uint8_t  *memory;
	struct   StatusRegisters sr;
} State6510;

State6510* state;
uint8_t *pBasicROM;
uint8_t *pKernalROM;
uint8_t *pCharROM;

/***************************************************************************** 
 ***  Addressing modes                                                     ***
 *****************************************************************************/
#define tZEROPAGE(op1)				(state->memory[(uint8_t) op1])
#define tZEROPAGEX(op1)				(state->memory[(uint16_t) (op1 + state->X)])
#define tZEROPAGEY(op1)				(state->memory[(uint16_t) (op1 + state->Y)])
#define tABSOLUTE(op1, op2)			(state->memory[(uint16_t) (op1 | (op2 << 8))])
#define tABSOLUTEX(op1, op2)		(state->memory[(uint16_t) ((op1 | (op2 << 8)) + state->X)])
#define tABSOLUTEY(op1, op2)		(state->memory[(uint16_t) ((op1 | (op2 << 8)) + state->Y)])
#define tINDIRECTX(IAL)				(state->memory[(uint16_t) (state->memory[IAL+state->X] | (state->memory[IAL+state->X+1] << 8))])
#define tINDIRECTY(IAL)				(state->memory[(uint16_t) (state->memory[IAL] | (state->memory[IAL+1] << 8)) + state->Y])

#define IMMEDIATE(op1)             (op1)
#define ZEROPAGE(op1)              (Peek(op1))
#define ZEROPAGEX(op1)             (Peek(op1 + state->X))
#define ZEROPAGEY(op1)             (Peek(op1 + state->Y))
#define ABSOLUTE(op1, op2)         (Peek(op1 | (op2 << 8)))
#define ABSOLUTEX(op1, op2)        (Peek((op1 | (op2 << 8)) + state->X))
#define ABSOLUTEY(op1, op2)        (Peek((op1 | (op2 << 8)) + state->Y))
// Indexed-Indirect addressing
// LDX #$00      ;X is loaded with zero (0),
// LDA ($02,X)   ;so the vector is calculated as $02 plus zero (0). The resulting vector is ($02). 
//
// If zero-page memory $02 contains 00 80,
// then the effective address from the vector (02) would be $8000. 
#define INDIRECTX(IAL)				(Peek((Peek(IAL+state->X) | Peek(IAL+1+state->X) << 8)))
// 00,IAL+X

// Indirect-indexed addressing
// LDY #$04      ;Y is loaded with four (4)
// LDA ($02),Y   ;the vector is given as ($02)
// 
// If zero-page memory $02 contains 00 80,
// then the effective address from the vector ($02) plus the offset (Y) would be $8004.
//
//                                        BAL=00,IAL   BAH=00,IAL+1
// BAH, BAL+Y
#define INDIRECTY(IAL)				(Peek((Peek(IAL) | (Peek(IAL+1) << 8)) + state->Y))
// Branch addressing
// When calculating branches a forward branch of 6 skips the following 6 bytes so,
// effectively the program counter points to the address that is 8 bytes beyond the address of the branch opcode;
//
// And a backward branch of $FA (256-6) goes to an address 4 bytes before the branch instruction.
#define BRANCH(offset)				(((offset) & (1<<7)) ? (state->PC = state->PC - (0xFF - (signed int)offset) + 1) : (state->PC = state->PC + offset + 2))

/*****************************************************************************
 *** Status Registers Macros                                               ***
 *****************************************************************************/
#define StatusRegisterNegative(op)			(state->sr.N = (((op & 0x80) == 0x80) ? 1 : 0)) // Negative
#define StatusRegisterZero(op)				(state->sr.Z = ((op & 0xFF) == 0) ? 1 : 0) // Zero (state->sr.Z = ((op & 0xFF) == 0)) // Zero
#define StatusRegisterCarry(op)				(state->sr.C = (op > 0xFF)) // Carry

 /*****************************************************************************
 *** PEEK: Read from Memory                                                ***
 ***                                                                       ***
 *** 6510 CPU PORT REGISTER 0x0001                                         ***
 *** bit 0 = LORAM  - BASIC ROM;			0 = RAM, 1 = ROM               ***
 *** bit 1 = HIRAM  - BASIC & KERNAL ROM;	0 = RAM, 1 = ROM               ***
 *** bit 2 = CHAREN - CHAR ROM;				0 = ROM, 1 = I/O               ***
 ***                                                                       ***
 *** bit 0 & 1 = 0; All ROM becomes RAM (bit 2 ignored)                    ***
 *****************************************************************************/
//#undef _DEBUG
#define _DEBUG
uint8_t Peek(uint16_t address)
{
#ifndef _DEBUG
	uint8_t memory_content;

	// 0x0000 - 0x00FF ZEROPAGE
	if ((address >= 0x0000) && (address <= 0x00FF))
		memory_content = state->memory[(uint8_t)address];
	// 0x0100 - 0x01FF STACK POINTER
	if ((address >= 0x0100) && (address <= 0x01FF))
		memory_content = state->memory[address];
	// 0x0200 - 0x9FFF RAM
	if ((address >= 0x0200) && (address <= 0x9FFF)) // RAM
		memory_content = state->memory[address];

	// 0xA000 - 0xBFFF BASIC ROM
	else if ((address >= 0xA000) && (address <= 0xBFFF)) // Basic ROM
	{
		// Check Data Register for ROM or RAM
		if ((state->memory[1] & 0x01) == 0x01)
			memory_content = pBasicROM[address - 0xA000];
		else
			memory_content = state->memory[address];
	}

	// 0xC000 - 0xCFFF RAM
	else if ((address >= 0xC000) && (address <= 0xCFFF)) // RAM
		memory_content = state->memory[address];

	// 0xD000 - 0xDFFF CHAR ROM - I/O
	else if ((address >= 0xD000) && (address <= 0xDFFF)) // Char ROM, I/O
	{
		// First check if all ROMs are enabled or disabled
		if ((state->memory[1] & 0x03) == 0x00) // All ROM's are RAM
			memory_content = state->memory[address];
		else if ((state->memory[1] & 0x04) == 0x04)
			// I/O is gekozen maar nog ff uitzoeken wat I/O is en waar het zit in het geheugen
			memory_content = state->memory[address];
		else
			memory_content = pCharROM[address - 0xD000];
	}

	// 0xE000 - 0xFFFF KERNAL ROM
	else if ((address >= 0xE000) && (address <= 0xFFFF)) // Kernal ROM, RAM
	{
		// First check if all ROMs are enabled or disabled
		if ((state->memory[1] & 0x03) == 0x00) // All ROM's are RAM
			memory_content = state->memory[address];
		else if ((state->memory[1] & 0x02) == 0x02)
			memory_content = pKernalROM[address - 0xE000];
		else
			memory_content = state->memory[address];
	}

	return memory_content;
#else
	return state->memory[address];
#endif
}

/*****************************************************************************
 *** POKE: Write to Memory                                                 ***
 ***                                                                       ***
 ***                                                                       ***
 ***                                                                       ***
 ***                                                                       ***
 ***                                                                       ***
 ***                                                                       ***
 ***                                                                       ***
 *****************************************************************************/
//uint8_t Poke(uint16_t address)
//{
//  check addressen die bezet zijn door: (waar je niet naar kunt schrijven)
//    de processor zelf
//    PLA
//    KERNAL
//    BASIC
//	  registers van de vic-ii en sid en cia
//	return 0;
//}

/*****************************************************************************
 *** ADC: Add memory to accumulator with carry                             ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _adc(opcode, pc_inc)												\
do {																		\
		uint16_t tmp;														\
		uint16_t tmp_value = opcode;										\
		uint16_t reg_a_read = state->A;										\
																			\
		tmp_value = opcode;													\
		reg_a_read = state->A;												\
																			\
		if (state->sr.D == 1)				 								\
		{																	\
			tmp = (reg_a_read & 0xf) + (tmp_value & 0xf) + state->sr.C;		\
			if (tmp > 0x9)													\
				tmp += 0x6;													\
			if (tmp <= 0x0f)												\
				tmp = (tmp & 0xf) + (reg_a_read & 0xf0) +					\
					(tmp_value & 0xf0);										\
			else															\
				tmp = (tmp & 0xf) + (reg_a_read & 0xf0) +					\
					(tmp_value & 0xf0) + 0x10;								\
			state->sr.Z = !((reg_a_read + tmp_value + state->sr.C) & 0xff);	\
			StatusRegisterNegative(tmp);									\
			state->sr.V = (((reg_a_read ^ tmp) & 0x80) &&					\
				!((reg_a_read ^ tmp_value) & 0x80));						\
			if ((tmp & 0x1f0) > 0x90)										\
				tmp += 0x60;												\
			state->sr.C = ((tmp & 0xff0) > 0xf0);							\
		}																	\
		else																\
		{																	\
			tmp = tmp_value + reg_a_read + state->sr.C;						\
			StatusRegisterZero(tmp);										\
			StatusRegisterNegative(tmp);									\
			state->sr.V = (!((reg_a_read ^ tmp_value) & 0x80) &&			\
				((reg_a_read ^ tmp) & 0x80));								\
			StatusRegisterCarry(tmp);										\
		}																	\
		state->A = (uint8_t)tmp;											\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)	
/*****************************************************************************
 *** AND: And memory with accumulator                                      ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _and(opcode, pc_inc)												\
do {																		\
		uint16_t answer = (uint16_t)(state->A & opcode);					\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		state->A = (uint8_t)answer;											\
		state->PC = (uint16_t)state->PC + (uint16_t)pc_inc;					\
	}																		\
while (0)
/*****************************************************************************
 *** ASL: Shift left one bit (memory or accumulator)                       ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 ***      dest = destination of the shift left							   ***
 *****************************************************************************/
#define _asl(opcode, pc_inc, destination)									\
do {																		\
		uint16_t answer = ((uint16_t)(opcode << 1));						\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		StatusRegisterCarry(answer);										\
		destination = (uint8_t)(answer & 0xff);								\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)
/*****************************************************************************
 *** CP: Compare memory and accumulator                                    ***
 ***     opcode = memory content                                           ***
 ***     inc_pc = Inc with no. of cycles                                   ***
 ***     dest = destination in memory                                      ***
 *****************************************************************************/
#define _cp(opcode, pc_inc, dest)											\
do {																		\
		uint16_t answer = (uint16_t)(dest - opcode);						\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		state->sr.C = ((opcode <= dest) ? 1 : 0);							\
		state->PC = state->PC + pc_inc;										\
}																			\
while (0)
/*****************************************************************************
 *** CMP: Compare memory and accumulator                                   ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _cmp(opcode, pc_inc)												\
do {																		\
		_cp(opcode, pc_inc, state->A);										\
	}																		\
while (0)
/*****************************************************************************
 *** CPX: Compare memory and index X                                       ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _cpx(opcode, pc_inc)												\
do {																		\
		_cp(opcode, pc_inc, state->X);										\
	}																		\
while (0)
/*****************************************************************************
 *** CPY: Compare memory and index Y                                       ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _cpy(opcode, pc_inc)												\
do {																		\
		_cp(opcode, pc_inc, state->Y);										\
	}																		\
while (0)
/*****************************************************************************
 *** DEC: Decrement memory by one                                          ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 ***      dest = destination of the decrement							   ***
 *****************************************************************************/
#define _dec(opcode, pc_inc, dest)											\
do {																		\
		uint8_t answer = (uint8_t)(opcode - 1);								\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		dest = (uint8_t)(answer & 0xff);									\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)
/*****************************************************************************
 *** DEX: Decrement index X by one                                         ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _dex()																\
do {																		\
		_dec(state->X, 1, state->X);										\
	}																		\
while (0)
/*****************************************************************************
 *** DEY: Decrement index Y by one                                         ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _dey()																\
do {																		\
		_dec(state->Y, 1, state->Y);										\
	}																		\
while (0)
/*****************************************************************************
 *** EOR: 'Exclusive OR' memory with accumulator                           ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _eor(opcode, pc_inc)												\
do {																		\
		uint8_t answer = (uint8_t)(state->A ^ opcode);						\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		state->A = (uint8_t)(answer & 0xff);								\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)
/*****************************************************************************
 *** INC: Increment memory by one                                          ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _inc(opcode, pc_inc, dest)											\
do {																		\
		uint8_t answer = (uint8_t)(opcode + 1);								\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		dest = (uint8_t)(answer & 0xff);									\
		state->PC = state->PC + pc_inc;										\
}																			\
while (0)
/*****************************************************************************
 *** INX: Increment memory by one                                          ***
 *****************************************************************************/
#define _inx()																\
do {																		\
		_inc(state->X, 1, state->X);										\
	}																		\
while (0)
/*****************************************************************************
 *** INY: Increment memory by one                                          ***
 *****************************************************************************/
#define _iny()																\
do {																		\
		_inc(state->Y, 1, state->Y);										\
	}																		\
while (0)
 /*****************************************************************************
 *** LD: Load register_content with memory                                 ***
 ***     register_content = content the 6510 registers (A, X, Y)           ***
 ***     memory = memory content                                           ***
 ***     inc_pc = Inc with no. of cycles                                   ***
 *****************************************************************************/
#define _ld(opcode, pc_inc, dest)											\
do {																		\
		uint16_t answer = (uint16_t) opcode;								\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		dest = (uint8_t)(answer & 0xff);									\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)
/*****************************************************************************
 *** LDA: Load accumulator with memory                                     ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _lda(opcode, pc_inc)												\
do {																		\
	_ld(opcode, pc_inc, state->A);											\
	}																		\
while (0)
/*****************************************************************************
 *** LDX: Load index X with memory                                         ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _ldx(opcode, pc_inc)												\
do {																		\
	_ld(opcode, pc_inc, state->X);											\
	}																		\
while (0)
/*****************************************************************************
 *** LDY: Load index Y with memory                                         ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _ldy(opcode, pc_inc)												\
do {																		\
	_ld(opcode, pc_inc, state->Y);											\
	}																		\
while (0)
/*****************************************************************************
 *** LSR: Logical Shift Right (memory or accumulator)                      ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _lsr(opcode, pc_inc, dest)											\
do {																		\
		uint8_t b0_before = opcode;											\
		uint16_t answer = ((uint16_t)(opcode >> 1));						\
		state->sr.N = 0;													\
		StatusRegisterZero(answer);											\
		state->sr.C = ((b0_before & 0x01) == 1);							\
		dest = (uint8_t)(answer & 0xff);									\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)
/*****************************************************************************
 *** ORA: OR memory with accumulator                                       ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _ora(opcode, pc_inc)												\
do {																		\
		uint16_t answer = (uint16_t)(state->A | opcode);					\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		state->A = (uint8_t)answer;											\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)
/*****************************************************************************
 *** ROL: Rotate one bit left (memory or accumulator)                      ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _rol(opcode, pc_inc, dest)											\
do {																		\
		uint16_t answer = (uint16_t)((opcode << 1) | state->sr.C);			\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		StatusRegisterCarry(answer);										\
		dest = (uint8_t)(answer & 0xff);									\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)
/*****************************************************************************
 *** ROR: Rotate one bit right (memory or accumulator)                     ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _ror(opcode, pc_inc, dest)											\
do {																		\
		uint16_t answer = (uint16_t)((opcode >> 1) | (state->sr.C << 7));	\
		StatusRegisterNegative(answer);										\
		StatusRegisterZero(answer);											\
		state->sr.C = ((opcode & 0x01) == 1) ? 1 : 0;						\
		dest = (uint8_t)(answer & 0xff);									\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)
/*****************************************************************************
 *** SBC: Subtract memory from accumulator with carry                      ***
 ***      opcode = memory content                                          ***
 ***      inc_pc = Inc with no. of cycles                                  ***
 *****************************************************************************/
#define _sbc(opcode,pc_inc)													\
do {																		\
		uint16_t src;														\
		uint16_t tmp;														\
		uint16_t reg_a_read;												\
																			\
		src = (int16_t)opcode;												\
		reg_a_read = (uint16_t)state->A;									\
		tmp = reg_a_read - src - ((state->sr.C & 0x1) ? 0 : 1);				\
																			\
		if (state->sr.D == 1)												\
		{																	\
			uint16_t tmp_a;													\
			tmp_a = (reg_a_read & 0xf) - (src & 0xf) -						\
				((state->sr.C & 0x1) ? 0 : 1);								\
			if (tmp_a & 0x10)												\
				tmp_a = ((tmp_a - 6) & 0xf) | ((reg_a_read & 0xf0) -		\
					(src & 0xf0) - 0x10);									\
			else															\
				tmp_a = (tmp_a & 0xf) | ((reg_a_read & 0xf0) -				\
					(src & 0xf0));											\
			if (tmp_a & 0x100)												\
				tmp_a -= 0x60;												\
			state->sr.C = (tmp < 0x100);									\
			StatusRegisterZero(tmp);										\
			StatusRegisterNegative(tmp);									\
			state->sr.V = (((reg_a_read ^ tmp) & 0x80) &&					\
				((reg_a_read ^ src) & 0x80));								\
			state->A = (uint8_t)(tmp_a & 0xff);								\
		}																	\
		else																\
		{																	\
			state->sr.C = (tmp < 0x100);									\
			StatusRegisterZero(tmp);										\
			StatusRegisterNegative(tmp);									\
			state->sr.V = (((reg_a_read ^ tmp) & 0x80) &&					\
				((reg_a_read ^ src) & 0x80));								\
			state->A = (uint8_t)(tmp & 0xff);								\
		}																	\
		state->PC = state->PC + pc_inc;										\
	}																		\
while (0)
/*****************************************************************************
 *** Disassemble the assembly code  									   ***
 *** pc is the current offset into the code								   ***
 ***																	   ***
 *** returns the number of bytes of the op								   ***
 *****************************************************************************/
int Disassemble6510Op(uint16_t pc)
{
  uint8_t opbytes;
  uint8_t cycles;
  opbytes = 1;
  cycles = 0;

  uint8_t code0 = Peek(pc);
  uint8_t code1 = Peek(pc + 1);
  uint8_t code2 = Peek(pc + 2);

  printf("%04X %02X ", pc, code0);
  switch (code0)
  {
	//                 FF FF FF  LDA FFFF
	case 0x00: printf("       BRK"); opbytes = 1; cycles = 7; break;
    case 0x01: printf("%02X     ORA ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x02: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0x03: printf("%02X     SLO ($%02X,X)", code1, code1); opbytes = 2; cycles = 8; break;
    case 0x04: printf("%02X     NOP $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x05: printf("%02X     ORA $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x06: printf("%02X     ASL $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0x07: printf("%02X     SLO $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0x08: printf("       PHP"); opbytes = 1; cycles = 3; break;
    case 0x09: printf("%02X     ORA #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x0A: printf("       ASL A"); opbytes = 1; cycles = 2; break;
    case 0x0B: printf("%02X     ANC #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x0C: printf("%02X %02X  NOP $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0x0D: printf("%02X %02X  ORA $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0x0E: printf("%02X %02X  ASL $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;
    case 0x0F: printf("%02X %02X  SLO $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;

    case 0x10: printf("%02X     BPL $%02X", code1, code1); opbytes = 2; cycles = 2 /* Add 1 if branch occurs to same page. Add 2 if branch occurs to different page */; break;
    case 0x11: printf("%02X     ORA ($%02X),Y", code1, code1); opbytes = 2; cycles = 5 /* Add 1 on page crossing */; break;
    case 0x12: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0x13: printf("%02X     SLO ($%02X),Y", code1, code1); opbytes = 2; cycles = 8; break;
    case 0x14: printf("%02X     NOP $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x15: printf("%02X     ORA $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x16: printf("%02X     ASL $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x17: printf("%02X     SLO $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x18: printf("       CLC"); opbytes = 1; cycles = 2; break;
    case 0x19: printf("%02X %02X  ORA $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0x1A: printf("       NOP"); opbytes = 1; cycles = 2; break;
    case 0x1B: printf("%02X %02X  SLO $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0x1C: printf("%02X %02X  NOP $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0x1D: printf("%02X %02X  ORA $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0x1E: printf("%02X %02X  ASL $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0x1F: printf("%02X %02X  SLO $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;

	case 0x20: printf("%02X %02X  JSR $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;
    case 0x21: printf("%02X     AND ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x22: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0x23: printf("%02X     RLA ($%02X,X)", code1, code1); opbytes = 2; cycles = 8; break;
    case 0x24: printf("%02X     BIT $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x25: printf("%02X     AND $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x26: printf("%02X     ROL $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0x27: printf("%02X     RLA $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0x28: printf("       PLP"); opbytes = 1; cycles = 4; break;
    case 0x29: printf("%02X     AND #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x2A: printf("       ROL A"); opbytes = 1; cycles = 2; break;
    case 0x2B: printf("%02X     ANC #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x2C: printf("%02X %02X  BIT $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0x2D: printf("%02X %02X  AND $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0x2E: printf("%02X %02X  ROL $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;
    case 0x2F: printf("%02X %02X  RLA $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;

	case 0x30: printf("%02X     BMI $%02X", code1, code1); opbytes = 2; cycles = 2 /* Add 1 if branch occurs to same page. Add 2 if branch occurs to different page */; break;
    case 0x31: printf("%02X     AND ($%02X),Y", code1, code1); opbytes = 2; cycles = 5 /* Add 1 if page boundary is crossed. */; break;
    case 0x32: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0x33: printf("%02X     RLA ($%02X),Y", code1, code1); opbytes = 2; cycles = 8; break;
    case 0x34: printf("%02X     NOP $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x35: printf("%02X     AND $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x36: printf("%02X     ROL $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x37: printf("%02X     RLA $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x38: printf("       SEC"); opbytes = 1; cycles = 2; break;
    case 0x39: printf("%02X %02X  AND $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 if page boundary is crossed. */; break;
    case 0x3A: printf("       NOP"); opbytes = 1; cycles = 2; break;
    case 0x3B: printf("%02X %02X  RLA $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0x3C: printf("%02X %02X  NOP $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0x3D: printf("%02X %02X  AND $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 if page boundary is crossed. */; break;
    case 0x3E: printf("%02X %02X  ROL $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0x3F: printf("%02X %02X  RLA $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;

	case 0x40: printf("       RTI"); opbytes = 1; cycles = 6; break;
    case 0x41: printf("%02X     EOR ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x42: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0x43: printf("%02X     SRE ($%02X,X)", code1, code1); opbytes = 2; cycles = 8; break;
    case 0x44: printf("%02X     NOP $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x45: printf("%02X     EOR $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x46: printf("%02X     LSR $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0x47: printf("%02X     SRE $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0x48: printf("       PHA"); opbytes = 1; cycles = 3; break;
    case 0x49: printf("%02X     EOR #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x4A: printf("       LSR A"); opbytes = 1; cycles = 2; break;
    case 0x4B: printf("%02X     ASR #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x4C: printf("%02X %02X  JMP $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 3; break;
    case 0x4D: printf("%02X %02X  EOR $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0x4E: printf("%02X %02X  LSR $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;
    case 0x4F: printf("%02X %02X  SRE $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;

	case 0x50: printf("%02X     BVC $%02X", code1, code1); opbytes = 2; cycles = 2 /* Add 1 if branch occurs to same page. Add 2 if branch occurs to different page */; break;
    case 0x51: printf("%02X     EOR ($%02X),Y", code1, code1); opbytes = 2; cycles = 5 /* Add 1 if page boundary is crossed. */; break;
    case 0x52: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0x53: printf("%02X     SRE ($%02X),Y", code1, code1); opbytes = 2; cycles = 8; break;
    case 0x54: printf("%02X     NOP $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x55: printf("%02X     EOR $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x56: printf("%02X     LSR $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x57: printf("%02X     SRE $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x58: printf("       CLI"); opbytes = 1; cycles = 2; break;
    case 0x59: printf("%02X %02X  EOR $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 if page boundary is crossed. */; break;
    case 0x5A: printf("       NOP"); opbytes = 1; cycles = 2; break;
    case 0x5B: printf("%02X %02X  SRE $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0x5C: printf("%02X %02X  NOP $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0x5D: printf("%02X %02X  EOR $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0x5E: printf("%02X %02X  LSR $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0x5F: printf("%02X %02X  SRE $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;

	case 0x60: printf("       RTS"); opbytes = 1; cycles = 6; break;
    case 0x61: printf("%02X     ADC ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x62: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0x63: printf("%02X     RRA ($%02X,X)", code1, code1); opbytes = 2; cycles = 8; break;
    case 0x64: printf("%02X     NOP $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x65: printf("%02X     ADC $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x66: printf("%02X     ROR $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0x67: printf("%02X     RRA $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0x68: printf("       PLA"); opbytes = 1; cycles = 4; break;
    case 0x69: printf("%02X     ADC #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x6A: printf("       ROR A"); opbytes = 1; cycles = 2; break;
    case 0x6B: printf("%02X     ARR #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x6C: printf("%02X %02X  JMP ($%02X%02X)", code1, code2, code2, code1); opbytes = 3; cycles = 5; break;
    case 0x6D: printf("%02X %02X  ADC $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0x6E: printf("%02X %02X  ROR $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;
    case 0x6F: printf("%02X %02X  RRA $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;

	case 0x70: printf("%02X     BVS $%02X", code1, code1); opbytes = 2; cycles = 2 /* Add 1 if branch occurs to same page. Add 2 if branch occurs to different page */; break;
    case 0x71: printf("%02X     ADC ($%02X),Y", code1, code1); opbytes = 2; cycles = 5  /* Add 1 on page crossing */; break;
    case 0x72: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0x73: printf("%02X     RRA ($%02X),Y", code1, code1); opbytes = 2; cycles = 8; break;
    case 0x74: printf("%02X     NOP $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x75: printf("%02X     ADC $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x76: printf("%02X     ROR $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x77: printf("%02X     RRA $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x78: printf("       SEI"); opbytes = 1; cycles = 2; break;
    case 0x79: printf("%02X %02X  ADC $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 if page boundary is crossed. */; break;
    case 0x7A: printf("       NOP"); opbytes = 1; cycles = 2; break;
    case 0x7B: printf("%02X %02X  RRA $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0x7C: printf("%02X %02X  NOP $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0x7D: printf("%02X %02X  ADC $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0x7E: printf("%02X %02X  ROR $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0x7F: printf("%02X %02X  RRA $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;

	case 0x80: printf("%02X     NOP #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x81: printf("%02X     STA ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x82: printf("%02X     NOP #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x83: printf("%02X     SAX ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x84: printf("%02X     STY $%02X", code1, code1); opbytes = 2; cycles = 3; break;
	case 0x85: printf("%02X     STA $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x86: printf("%02X     STX $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x87: printf("%02X     SAX $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0x88: printf("       DEY"); opbytes = 1; cycles = 2; break;
    case 0x89: printf("%02X     NOP #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x8A: printf("       TXA"); opbytes = 1; cycles = 2; break;
    case 0x8B: printf("%02X     ANE #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0x8C: printf("%02X %02X  STY $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0x8D: printf("%02X %02X  STA $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0x8E: printf("%02X %02X  STX $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0x8F: printf("%02X %02X  SAX $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;

	case 0x90: printf("%02X     BCC #%02X", code1, code1); opbytes = 2; cycles = 2 /* Add 1 if branch occurs to same page. Add 2 if branch occurs to different page */;break;
    case 0x91: printf("%02X     STA ($%02X),Y", code1, code1); opbytes = 2; cycles = 6; break;
    case 0x92: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0x93: printf("%02X %02X  SHA $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 5; break;
    case 0x94: printf("%02X     STY $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x95: printf("%02X     STA $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x96: printf("%02X     STX $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x97: printf("%02X     SAX $%02X,Y", code1, code1); opbytes = 2; cycles = 4; break;
    case 0x98: printf("       TYA"); opbytes = 1; cycles = 2; break;
    case 0x99: printf("%02X %02X  STA $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 5; break;
    case 0x9A: printf("       TXS"); opbytes = 1; cycles = 2; break;
    case 0x9B: printf("%02X %02X  SHS $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 5; break;
    case 0x9C: printf("%02X %02X  SHY $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 5; break;
    case 0x9D: printf("%02X %02X  STA $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 5; break;
    case 0x9E: printf("%02X %02X  SHX $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 5; break;
    case 0x9F: printf("%02X %02X  SHA $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 5; break;

	case 0xA0: printf("%02X     LDY #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xA1: printf("%02X     LDA ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0xA2: printf("%02X     LDX #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xA3: printf("%02X     LAX ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0xA4: printf("%02X     LDY $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0xA5: printf("%02X     LDA $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0xA6: printf("%02X     LDX $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0xA7: printf("%02X     LAX $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0xA8: printf("       TAY"); opbytes = 1; cycles = 2; break;
    case 0xA9: printf("%02X     LDA #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xAA: printf("       TAX"); opbytes = 1; cycles = 2; break;
    case 0xAB: printf("%02X     LXA #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xAC: printf("%02X %02X  LDY $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0xAD: printf("%02X %02X  LDA $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0xAE: printf("%02X %02X  LDX $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0xAF: printf("%02X %02X  LAX $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;

	case 0xB0: printf("%02X     BCS $%02X", code1, code1); opbytes = 2; cycles = 2 /* Add 1 if branch occurs to same page. Add 2 if branch occurs to different page */; break;
    case 0xB1: printf("%02X     LDA ($%02X),Y", code1, code1); opbytes = 2; cycles = 5 /* Add 1 on page crossing */; break;
    case 0xB2: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0xB3: printf("%02X     LAX ($%02X),Y", code1, code1); opbytes = 2; cycles = 5 /* Add 1 on page crossing */; break;
    case 0xB4: printf("%02X     LDY $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0xB5: printf("%02X     LDA $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0xB6: printf("%02X     LDX $%02X,Y", code1, code1); opbytes = 2; cycles = 4; break;
    case 0xB7: printf("%02X     LAX $%02X,Y", code1, code1); opbytes = 2; cycles = 4; break;
    case 0xB8: printf("       CLV"); opbytes = 1; cycles = 2; break;
    case 0xB9: printf("%02X %02X  LDA $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xBA: printf("       TSX"); opbytes = 1; cycles = 2; break;
    case 0xBB: printf("%02X %02X  LAE $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xBC: printf("%02X %02X  LDY $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xBD: printf("%02X %02X  LDA $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xBE: printf("%02X %02X  LDX $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xBF: printf("%02X %02X  LAX $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;

	case 0xC0: printf("%02X     CPY #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xC1: printf("%02X     CMP ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0xC2: printf("%02X     NOP #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xC3: printf("%02X     DCP ($%02X,X)", code1, code1); opbytes = 2; cycles = 8; break;
    case 0xC4: printf("%02X     CPY $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0xC5: printf("%02X     CMP $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0xC6: printf("%02X     DEC $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0xC7: printf("%02X     DCP $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0xC8: printf("       INY"); opbytes = 1; cycles = 2; break;
    case 0xC9: printf("%02X     CMP #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xCA: printf("       DEX"); opbytes = 1; cycles = 2; break;
    case 0xCB: printf("%02X     SBX #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xCC: printf("%02X %02X  CPY $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0xCD: printf("%02X %02X  CMP $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0xCE: printf("%02X %02X  DEC $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;
    case 0xCF: printf("%02X %02X  DCP $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;

	case 0xD0: printf("%02X     BNE $%02X", code1, code1); opbytes = 2; cycles = 2 /* Add 1 if branch occurs to same page. Add 2 if branch occurs to different page */; break;
    case 0xD1: printf("%02X     CMP ($%02X),Y", code1, code1); opbytes = 2; cycles = 5 /* Add 1 on page crossing */; break;
    case 0xD2: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0xD3: printf("%02X     DCP ($%02X),Y", code1, code1); opbytes = 2; cycles = 8; break;
    case 0xD4: printf("%02X     NOP $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0xD5: printf("%02X     NOP $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0xD6: printf("%02X     NOP $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0xD7: printf("%02X     DCP $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0xD8: printf("       CLD"); opbytes = 1; cycles = 2; break;
    case 0xD9: printf("%02X %02X  CMP $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xDA: printf("       NOP"); opbytes = 1; cycles = 2; break;
    case 0xDB: printf("%02X %02X  DCP $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0xDC: printf("%02X %02X  NOP $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xDD: printf("%02X %02X  CMP $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xDE: printf("%02X %02X  DEC $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0xDF: printf("%02X %02X  DCP $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;

	case 0xE0: printf("%02X     CPX #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xE1: printf("%02X     SBC ($%02X,X)", code1, code1); opbytes = 2; cycles = 6; break;
    case 0xE2: printf("%02X     NOP #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xE3: printf("%02X     ISB ($%02X,X)", code1, code1); opbytes = 2; cycles = 8; break;
    case 0xE4: printf("%02X     CPX $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0xE5: printf("%02X     SBC $%02X", code1, code1); opbytes = 2; cycles = 3; break;
    case 0xE6: printf("%02X     INC $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0xE7: printf("%02X     IBC $%02X", code1, code1); opbytes = 2; cycles = 5; break;
    case 0xE8: printf("       INX"); opbytes = 1; cycles = 2; break;
    case 0xE9: printf("%02X     SBC #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xEA: printf("       NOP"); opbytes = 1; cycles = 2; break;
    case 0xEB: printf("%02X     SBC #$%02X", code1, code1); opbytes = 2; cycles = 2; break;
    case 0xEC: printf("%02X %02X  CPX $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 5; break;
    case 0xED: printf("%02X %02X  SBC $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 4; break;
    case 0xEE: printf("%02X %02X  INC $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;
    case 0xEF: printf("%02X %02X  ISB $%02X%02X", code1, code2, code2, code1); opbytes = 3; cycles = 6; break;

	case 0xF0: printf("%02X     BEQ $%02X", code1, code1); opbytes = 2; cycles = 2 /* Add 1 if branch occurs to same page. Add 2 if branch occurs to different page */; break;
    case 0xF1: printf("%02X     SBC ($%02X),Y", code1, code1); opbytes = 2; cycles = 5 /* Add 1 on page crossing */; break;
    case 0xF2: printf("       JAM"); opbytes = 1; cycles = 0; break;
    case 0xF3: printf("%02X     ISB ($%02X),Y", code1, code1); opbytes = 2; cycles = 8; break;
    case 0xF4: printf("%02X     NOP $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0xF5: printf("%02X     SBC $%02X,X", code1, code1); opbytes = 2; cycles = 4; break;
    case 0xF6: printf("%02X     INC $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0xF7: printf("%02X     ISB $%02X,X", code1, code1); opbytes = 2; cycles = 6; break;
    case 0xF8: printf("       SED"); opbytes = 1; cycles = 2; break;
    case 0xF9: printf("%02X %02X SBC $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xFA: printf("       NOP"); opbytes = 1; cycles = 2; break;
    case 0xFB: printf("%02X %02X ISB $%02X%02X,Y", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0xFC: printf("%02X %02X NOP $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xFD: printf("%02X %02X SBC $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 4 /* Add 1 on page crossing */; break;
    case 0xFE: printf("%02X %02X INC $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
    case 0xFF: printf("%02X %02X ISB $%02X%02X,X", code1, code2, code2, code1); opbytes = 3; cycles = 7; break;
  }
  printf("\n");

  return opbytes;
}

void UnimplementedInstruction()
{
	//pc will have advanced one, so undo that
	printf ("Error: Unimplemented instruction\n");
	//state->PC--;
	state->PC++;
	//Disassemble6510Op(state->PC);
	//printf("\n");
	//exit(1);
}

int Emulate6510Op(State6510* state)
{
	uint8_t opcode0 = Peek(state->PC);
	uint8_t opcode1 = Peek(state->PC + 1);
	uint8_t opcode2 = Peek(state->PC + 2);

//	Disassemble6510Op(state->PC);

	switch(opcode0)
	{
		case 0x00: // BRK (Implied/Stack)
			{
				state->PC = state->PC + 2; // PC + 2 to Stack,
				state->memory[state->SP] = (state->PC >> 8) & 0xFF; // SPH
				state->memory[state->SP - 1] = state->PC & 0xFF; // SPL
				state->SP = state->SP - 2;
				// Set the BREAK bit in the Processor Status Register 
				state->sr.B = 1;
				// Processor Status Register to Stack NV_BDIZC
				state->memory[state->SP] = (state->sr.C | state->sr.Z << 1 | state->sr.I << 2 | state->sr.D << 3 | state->sr.B << 4 | state->sr.dc << 5 | state->sr.V << 6 | state->sr.N << 7); // SPH
				state->SP = state->SP - 1;
				// Set the BREAK bit in the stack at SP -1
				//state->memory[state->SP + 1] = state->memory[state->SP + 1] | 0x40; // Removed because redundant, if B is set before putting SR on stack
				state->PC = 0xfffe; // transfers control to the interrupt vector
				exit(1);
			}
			break;
		case 0x01: // ORA ($FF,X) (Indexed Indirect,X)	A OR M -> A (A V M -> A)
			{
				_ora(INDIRECTX(opcode1), 2);
			}
			break;
		case 0x02: UnimplementedInstruction(); break;
		case 0x03: UnimplementedInstruction(); break;
		case 0x04: UnimplementedInstruction(); break;
		case 0x05: // ORA $FF (Zeropage)  A OR M -> A
			{
				_ora(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0x06: // ASL $FF (Zeropage) C <- 76543210 <- 0
			{
				//tZEROPAGE(opcode1) = _asl(ZEROPAGE(opcode1), 2);
				_asl(ZEROPAGE(opcode1), 2, tZEROPAGE(opcode1));
			}
			break;
		case 0x07: UnimplementedInstruction(); break;
		case 0x08: // PHP (Implied/Stack) P to Stack
			{
				// Processor Status Register to Stack
				state->memory[state->SP] = (state->sr.C | state->sr.Z << 1 | state->sr.I << 2 | state->sr.D << 3 | state->sr.B << 4 | state->sr.dc << 5 | state->sr.V << 6 | state->sr.N << 7); // SPH
				state->SP = state->SP - 1;
				state->PC = state->PC + 1;
			}
			break;
		case 0x09: // ORA #$FF (Immediate) A OR M -> A (A V M -> A)
			{
				_ora(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0x0A: // ASL A (Accumulator) C <- 76543210 <- 0
			{
				_asl(state->A, 1, state->A); // read/modify/write instruction
			}
			break;
		case 0x0B: UnimplementedInstruction(); break;
		case 0x0C: UnimplementedInstruction(); break;
		case 0x0D: // ORA $FFFF (Absolute)  A OR M -> A (A V M -> A)
			{
				_ora(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0x0E: // ASL $FFFF (Absolute) C <- 76543210 <- 0
			{
				_asl(ABSOLUTE(opcode1, opcode2), 3, tABSOLUTE(opcode1, opcode2));
			}
			break;
		case 0x0F: UnimplementedInstruction(); break;

		case 0x10: // BPL $FFFF (Relative) Branch on N = 0
			{
				(state->sr.N == 0) ? (state->PC = BRANCH(opcode1)) : (state->PC = state->PC + 2);
			}
			break;
		case 0x11: // ORA ($FF),Y (Indirect Indexed,Y)	A OR M -> A (A V M -> A)
			{
				_ora(INDIRECTY(opcode1), 2);
			}
			break;
		case 0x12: UnimplementedInstruction(); break;
		case 0x13: UnimplementedInstruction(); break;
		case 0x14: UnimplementedInstruction(); break;
		case 0x15: // ORA $FF,X (Zeropage,X) A OR M -> A (A V M -> A)
			{
				_ora(ZEROPAGEX(opcode1), 2);
			}
			break;
		case 0x16: // ASL $FF,X (Zeropage,X) C <- 76543210 <- 0
			{
				_asl(ZEROPAGEX(opcode1), 2, tZEROPAGEX(opcode1));
			}
			break;
		case 0x17: UnimplementedInstruction(); break;
		case 0x18: // CLC (Implied) 0 -> C
			{
				state->sr.C = 0;
				state->PC = state->PC + 1;
			}
			break;
		case 0x19: // ORA $FFFF,Y (Absolute,Y) A V M -> A
			{
				_ora(ABSOLUTEY(opcode1, opcode2), 3);
			}
			break;
		case 0x1A: UnimplementedInstruction(); break;
		case 0x1B: UnimplementedInstruction(); break;
		case 0x1C: UnimplementedInstruction(); break;
		case 0x1D: // ORA $FFFF,X (Absolute,X) A OR M -> A (A V M -> A)
			{
				_ora(ABSOLUTEX(opcode1, opcode2), 3);
			}
			break;
		case 0x1E: // ASL $FFFF,X (Absolute,X) C <- 76543210 <- 0
			{
				_asl(ABSOLUTEX(opcode1, opcode2), 3, tABSOLUTEX(opcode1, opcode2));
			}
			break;
		case 0x1F: UnimplementedInstruction(); break;

		case 0x20: // JSR $XXXX (Absolute) PC + 2 to Stack
			{
				state->PC = state->PC + 2;
				state->memory[state->SP] = state->PC & 0xFF; // SPL
				state->memory[state->SP - 1] = (state->PC >> 8) & 0xFF; // SPH
				state->SP = state->SP - 2;
				state->PC = (uint16_t) (opcode1 | (opcode2 << 8));
			}
			break;
		case 0x21: // AND ($FF,X) (Indexed Indirect,X)	A AND M -> A (A /\ M -> A)
			{
				_and(INDIRECTX(opcode1), 2);
			}
			break;
		case 0x22: UnimplementedInstruction(); break;
		case 0x23: UnimplementedInstruction(); break;
		case 0x24: // BIT $FF (ZeroPage) A AND M -> Z (M /\ A) M7 -> N, M6 -> V
			{
				uint8_t b7 = 0x01 & (ZEROPAGE(opcode1) >> 7); // bit 7 affects the N flag
				uint8_t b6 = 0x01 & (ZEROPAGE(opcode1) >> 6); // bit 6 affects the V flag
				uint16_t answer = (uint16_t) (state->A & ZEROPAGE(opcode1));
				state->sr.N = ((b7 & 0x01) == 1);
				state->sr.V = ((b6 & 0x01) == 1);
				state->sr.Z = ((answer & 0xFF) == 0); // Zero
				state->PC = state->PC + 2;
			}
			break;
		case 0x25: // AND $FF (Zeropage) A AND M -> A (A /\ M -> A)
			{
				_and(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0x26: // ROL $FF (ZeroPage) <- 76543210 <- C <-
			{
				_rol(ZEROPAGE(opcode1), 2, tZEROPAGE(opcode1));
			}
			break;
		case 0x27: UnimplementedInstruction(); break;
		case 0x28: // PLP (Implied/Stack) P from Stack
			{
				uint8_t psr = Peek(state->SP + 1);
				// Processor Status Register from Stack
				state->sr.C = (0x01 == (psr & 0x01));
				state->sr.Z = (0x02 == (psr & 0x02));
				state->sr.I = (0x04 == (psr & 0x04));
				state->sr.D = (0x08 == (psr & 0x08));
				state->sr.B = (0x10 == (psr & 0x10));
				state->sr.dc = (0x20 == (psr & 0x20));
				state->sr.V = (0x40 == (psr & 0x40));
				state->sr.N = (0x80 == (psr & 0x80));
				state->SP = state->SP + 1;
				state->PC = state->PC + 1;
			}
			break;
		case 0x29: // AND #$FF (Immediate) A AND M -> A (A /\ M -> A)
			{
				_and(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0x2A: // ROL A (Accumulator) <- 76543210 <- C <-
			{
				_rol(state->A, 1, state->A);
			}
			break;
		case 0x2B: UnimplementedInstruction(); break;
		case 0x2C: // BIT $FFFF (Absolute) A AND M -> Z (M /\ A) M7 -> N, M6 -> V
			{
				uint8_t b7 = 0x01 & (ABSOLUTE(opcode1, opcode2) >> 7); // bit 7 affects the N flag
				uint8_t b6 = 0x01 & (ABSOLUTE(opcode1, opcode2) >> 6); // bit 6 affects the V flag
				uint16_t answer = (uint16_t) (state->A & ABSOLUTE(opcode1, opcode2));
				state->sr.N = ((b7 & 0x01) == 1);
				state->sr.V = ((b6 & 0x01) == 1);
				state->sr.Z = ((answer & 0xFF) == 0); // Zero
				state->PC = state->PC + 3;
			}
			break;
		case 0x2D: // AND $FFFF (Absolute)  A AND M -> A (A /\M -> A)
			{
				_and(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0x2E: // ROL $FFFF (Absolute) <- 76543210 <- C <-
			{
				_rol(ABSOLUTE(opcode1, opcode2), 3, tABSOLUTE(opcode1, opcode2));
			}
			break;
		case 0x2F: UnimplementedInstruction(); break;

		case 0x30: // BMI $FFFF Branch on N = 1
			{
				(state->sr.N == 1) ? (state->PC = BRANCH(opcode1)) : (state->PC = state->PC + 2);
			}
			break;
		case 0x31: // AND ($FF),Y (Indirect Indexed,Y)	A AND M -> A (A /\ M -> A)
			{
				_and(INDIRECTX(opcode1), 2);
			}
			break;
		case 0x32: UnimplementedInstruction(); break;
		case 0x33: UnimplementedInstruction(); break;
		case 0x34: UnimplementedInstruction(); break;
		case 0x35: // AND $FF,X (Zeropage,X) A AND M -> A (A /\ M -> A)
			{
				_and(ZEROPAGEX(opcode1), 2);
			}
			break;
		case 0x36: // ROL $FF,X (ZeroPage,X) <- 76543210 <- C <-
			{
				_rol(tZEROPAGEX(opcode1), 2, tZEROPAGEX(opcode1));
			}
			break;
		case 0x37: UnimplementedInstruction(); break;
		case 0x38: // SEC (Implied) 1 -> C
			{
				state->sr.C = 1;
				state->PC = state->PC + 1;
			}
			break;
		case 0x39: // AND $FFFF,Y (Absolute,Y) A AND M -> A (A /\ M -> A)
			{
				_and(ABSOLUTEY(opcode1, opcode2), 3);
			}
			break;
		case 0x3A: UnimplementedInstruction(); break;
		case 0x3B: UnimplementedInstruction(); break;
		case 0x3C: UnimplementedInstruction(); break;
		case 0x3D: // AND $FFFF,X (Absolute,X) A AND M -> A (A /\ M -> A)
			{
				_and(ABSOLUTEX(opcode1, opcode2), 3);
			}
			break;
		case 0x3E: // ROL $FFFF,X (Absolute,X) <- 76543210 <- C <-
			{
				_rol(ABSOLUTEX(opcode1, opcode2), 3, tABSOLUTEX(opcode1, opcode2));
			}
			break;
		case 0x3F: UnimplementedInstruction(); break;

		case 0x40: // RTI (Implied) Return from Interrupt
			{
				uint8_t psr = state->memory[state->SP + 1];
				// Processor Status Register from Stack
				state->sr.C = (0x01 == (psr & 0x01));
				state->sr.Z = (0x02 == (psr & 0x02));
				state->sr.I = (0x04 == (psr & 0x04));
				state->sr.D = (0x08 == (psr & 0x08));
				state->sr.B = (0x10 == (psr & 0x10));
				state->sr.dc = (0x20 == (psr & 0x20));
				state->sr.V = (0x40 == (psr & 0x40));
				state->sr.N = (0x80 == (psr & 0x80));
				state->SP = state->SP + 1;
				// PC from Stack
				state->PC = state->memory[state->SP + 1] | (state->memory[state->SP + 2] << 8);
				state->SP = state->SP + 2;
			}
			break;
		case 0x41: // EOR ($FF,X) (Indexed Indirect,X)	A EOR M -> A
			{
				_eor(INDIRECTX(opcode1), 2);
			}
			break;
		case 0x42: UnimplementedInstruction(); break;
		case 0x43: UnimplementedInstruction(); break;
		case 0x44: UnimplementedInstruction(); break;
		case 0x45: // EOR $FF (Zeropage) A EOR M -> A
			{
				_eor(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0x46: // LSR $FF (Zeropage) 0 -> 76543210 -> C
			{
				_lsr(ZEROPAGE(opcode1), 2, tZEROPAGE(opcode1));
			}
			break;
		case 0x47: UnimplementedInstruction(); break;
		case 0x48: // PHA (Implied/Stack) A to Stack
			{
				state->memory[state->SP] = (uint8_t) state->A;
				state->SP = state->SP - 1;
				state->PC = state->PC + 1;
			}
			break;
		case 0x49: // EOR #$FF (Immediate) A EOR M -> A
			{
				_eor(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0x4A: // LSR A (Accumulator) 0 -> 76543210 -> C
			{
				_lsr(state->A, 1, state->A);
			}
			break;
		case 0x4B: UnimplementedInstruction(); break;
		case 0x4C: // JMP $XXXX (Absolute) (PC + 1) -> PCL, (PC + 2) -> PCH
			{
				state->PC = (uint16_t)(opcode1 | (opcode2 << 8));
			}
			break;
		case 0x4D: // EOR $FFFF (Absolute)  A EOR M -> A
			{
				_eor(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0x4E: // LSR $FFFF (Absolute) 0 -> 76543210 -> C
			{
				_lsr(ABSOLUTE(opcode1, opcode2), 3, tABSOLUTE(opcode1, opcode2));
			}
			break;
		case 0x4F: UnimplementedInstruction(); break;

		case 0x50: // BVC $FFFF (Relative) Branch on V = 0
			{
				(state->sr.V == 0) ? (state->PC = BRANCH(opcode1)) : (state->PC = state->PC + 2);
			}
			break;
		case 0x51: // EOR ($FF),Y (Indirect Indexed,Y) A EOR M -> A
			{
				_eor(INDIRECTY(opcode1), 2);
			}
			break;
		case 0x52: UnimplementedInstruction(); break;
		case 0x53: UnimplementedInstruction(); break;
		case 0x54: UnimplementedInstruction(); break;
		case 0x55: // EOR $FF,X (Zeropage,X) A EOR M -> A
			{
				_eor(ZEROPAGEX(opcode1), 2);
			}
			break;
		case 0x56: // LSR $FF,X (Zeropage,X) 0 -> 76543210 -> C
			{
				_lsr(ZEROPAGEX(opcode1), 2, tZEROPAGEX(opcode1));
			}
			break;
		case 0x57: UnimplementedInstruction(); break;
		case 0x58: // CLI (Implied) 0 -> I
			{
				state->sr.I = 0;
				state->PC = state->PC + 1;
			}
			break;
		case 0x59: // EOR $FFFF,Y (Absolute,Y) A EOR M -> A
			{
				_eor(ABSOLUTEY(opcode1, opcode2), 3);
			}
			break;
		case 0x5A: UnimplementedInstruction(); break;
		case 0x5B: UnimplementedInstruction(); break;
		case 0x5C: UnimplementedInstruction(); break;
		case 0x5D: // EOR $FFFF,X (Absolute,X) A EOR M -> A
			{
				_eor(ABSOLUTEX(opcode1, opcode2), 3);
			}
			break;
		case 0x5E: // LSR $FFFF,X (Absolute,X) 0 -> 76543210 -> C
			{
				_lsr(ABSOLUTEX(opcode1, opcode2), 3, tABSOLUTEX(opcode1, opcode2));
			}
			break;
		case 0x5F: UnimplementedInstruction(); break;

		case 0x60: // RTS (Implied/Stack) Return from Subroutine
			{
				// PC from Stack
				//state->PC = state->memory[state->SP + 1] | (state->memory[state->SP + 2] << 8);
				state->PC = (Peek(state->SP + 2) | (Peek(state->SP + 1) << 8));
				state->SP = state->SP + 2;
				// PC + 1
				state->PC = state->PC + 1;
			}
			break;
		case 0x61: // ADC ($FF,X) (Indirect,X) A + M + C -> A, C
			{
				_adc(INDIRECTX(opcode1), 2);
			}
			break;
		case 0x62: UnimplementedInstruction(); break;
		case 0x63: UnimplementedInstruction(); break;
		case 0x64: UnimplementedInstruction(); break;
		case 0x65: // ADC $FF (ZeroPage) A + M + C -> A, C
			{
				_adc(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0x66: // ROR $FF (ZeroPage) b0 -> C -> 76543210 -> C
			{
				_ror(ZEROPAGE(opcode1), 2, tZEROPAGE(opcode1));
			}
			break;
		case 0x67: UnimplementedInstruction(); break;
		case 0x68: // PLA (Implied/Stack) A from Stack
			{
				state->A = Peek(state->SP + 1);
				StatusRegisterNegative(state->A);
				StatusRegisterZero(state->A);
				state->SP = state->SP + 1;
				state->PC = state->PC + 1;
			}
			break;
		case 0x69: // ADC #$FF (Immediate) A + M + C -> A, C
			{
				_adc(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0x6A: // ROR A (Accumulator) b0 -> C-> 76543210 -> C
			{
				_ror(state->A, 1, state->A);
			}
			break;
		case 0x6B: UnimplementedInstruction(); break;
		case 0x6C: // JMP ($XXXX) (Abs.Indirect) (PC + 1) -> PCL, (PC + 2) -> PCH
			{
				// Load ADL from address $XXXX address and load ADH from address $XXXX + 1
				state->PC = Peek((uint16_t)(opcode1 | (opcode2 << 8))) | (Peek((uint16_t)((opcode1 | (opcode2 << 8)) + 1)) << 8);
			}
			break;
		case 0x6D: // ADC $FFFF (Absolute) A + M + C -> A, C
			{
				_adc(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0x6E: // ROR $FFFF (Absolute) b0 -> C -> 76543210 -> C
			{
				_ror(ABSOLUTE(opcode1, opcode2), 3, tABSOLUTE(opcode1, opcode2));
			}
			break;
		case 0x6F: UnimplementedInstruction(); break;

		case 0x70: // BVS $FFFF (Relative) Branch on V = 1
			{
				(state->sr.V == 1) ? (state->PC = BRANCH(opcode1)) : (state->PC = state->PC + 2);
			}
			break;
		case 0x71: // ADC ($FF),Y ((Indirect),Y) A + M + C -> A, C
			{
				_adc(INDIRECTY(opcode1), 2);
			}
			break;
		case 0x72: UnimplementedInstruction(); break;
		case 0x73: UnimplementedInstruction(); break;
		case 0x74: UnimplementedInstruction(); break;
		case 0x75: // ADC $FF,X (ZeroPage,X) A + M + C -> A, C
			{
				_adc(ZEROPAGEX(opcode1), 2);
			}
			break;
		case 0x76: // ROR $FF,X (ZeroPage,X) b0 -> C -> 76543210 -> C
			{
				_ror(ZEROPAGEX(opcode1), 2, tZEROPAGEX(opcode1));
			}
			break;
		case 0x77: UnimplementedInstruction(); break;
		case 0x78: // SEI (Implied) 1 -> I
			{
				state->sr.I = 1;
				state->PC = state->PC + 1;
			}
			break;
		case 0x79: // ADC $FFFF,Y (Absolute,Y) A + M + C -> A, C
			{
				_adc(ABSOLUTEY(opcode1, opcode2), 3);
			}
			break;
		case 0x7A: UnimplementedInstruction(); break;
		case 0x7B: UnimplementedInstruction(); break;
		case 0x7C: UnimplementedInstruction(); break;
		case 0x7D: // ADC $FFFF,X (Absolute,X) A + M + C -> A, C
			{
				_adc(ABSOLUTEX(opcode1, opcode2), 3);
			}
			break;
		case 0x7E: // ROR $FFFF,X (Absolute,X) b0 -> C -> 76543210 -> C
			{
				_ror(ABSOLUTEX(opcode1, opcode2), 3, tABSOLUTEX(opcode1, opcode2));
			}
			break;
		case 0x7F: UnimplementedInstruction(); break;

		case 0x80: UnimplementedInstruction(); break;
		case 0x81: // STA ($FF,X) (Indirect,X) A -> M
			{
				tINDIRECTX(opcode1) = (uint8_t) state->A; // LSB
				state->PC = state->PC + 2;
			}
			break;
		case 0x82: UnimplementedInstruction(); break;
		case 0x83: UnimplementedInstruction(); break;
		case 0x84: // STY $FF (ZeroPage) Y -> M
			{
				tZEROPAGE(opcode1) = (uint8_t) state->Y; // LSB
				state->PC = state->PC + 2;
			}
			break;
		case 0x85: // STA $FF (ZeroPage) A -> M
			{
				tZEROPAGE(opcode1) = (uint8_t)state->A;
				state->PC = state->PC + 2;
		}
			break;
		case 0x86: // STX $FF (ZeroPage) X -> M
			{
				tZEROPAGE(opcode1) = (uint8_t) state->X; // LSB
				state->PC = state->PC + 2;
			}
			break;
		case 0x87: UnimplementedInstruction(); break;
		case 0x88: // DEY (Implied) Y - 1 -> Y
			{
				_dey();
			}
			break;
		case 0x89: UnimplementedInstruction(); break;
		case 0x8A: // TXA (Implied) X -> A
			{
				uint16_t answer = (uint16_t) state->X;
				StatusRegisterNegative(answer);
				StatusRegisterZero(answer);
				state->A = (uint8_t) answer; // LSB
				state->PC = state->PC + 1;
			}
			break;
		case 0x8B: UnimplementedInstruction(); break;
		case 0x8C: // STY $FFFF (Absolute) A -> M
			{
				tABSOLUTE(opcode1, opcode2) = (uint8_t) state->Y; // LSB
				state->PC = state->PC + 3;
			}
			break;
		case 0x8D: // STA $FFFF (Absolute) A -> M
			{
				tABSOLUTE(opcode1, opcode2) = (uint8_t) state->A; // LSB
				state->PC = state->PC + 3;
			}
			break;
		case 0x8E: // STX $FFFF (Absolute) X -> M
			{
				tABSOLUTE(opcode1, opcode2) = (uint8_t) state->X; // LSB
				state->PC = state->PC + 3;
			}
			break;
		case 0x8F: UnimplementedInstruction(); break;

		case 0x90: // BCC $FFFF (Relative) Branch on C = 0
			{
				if (state->sr.C == 0)
					BRANCH(opcode1);
				else
					state->PC = state->PC + 2;
			}
			break;
		case 0x91: // STA ($FF),Y (Indirect),Y A -> M
			{
				tINDIRECTY(opcode1) = (uint8_t) state->A; // LSB
				state->PC = state->PC + 2;
			}
			break;
		case 0x92: UnimplementedInstruction(); break;
		case 0x93: UnimplementedInstruction(); break;
		case 0x94: // STY $FF,X (ZeroPage,X) Y -> M
			{
				tZEROPAGEX(opcode1) = (uint8_t) state->Y; // LSB
				state->PC = state->PC + 2;
			}
			break;
		case 0x95: // STA $FF,X (ZeroPage,X) A -> M
			{
				tZEROPAGEX(opcode1) = (uint8_t)state->A; // LSB
				state->PC = state->PC + 2;
			}
			break;
		case 0x96: // STX $FF,Y (ZeroPage,Y) X -> M
			{
				tZEROPAGEY(opcode1) = (uint8_t) state->X; // LSB
				state->PC = state->PC + 2;
			}
			break;
		case 0x97: UnimplementedInstruction(); break;
		case 0x98: // TYA (Implied) Y -> A
			{
				uint16_t answer = (uint16_t) state->Y;
				StatusRegisterNegative(answer);
				StatusRegisterZero(answer);
				state->A = (uint8_t) (answer & 0xff); // LSB
				state->PC = state->PC + 1;
			}
			break;
		case 0x99: // STA $FFFF,Y (Absolute,Y) A -> M
			{
				tABSOLUTEY(opcode1, opcode2) = (uint8_t) state->A; // LSB
				state->PC = state->PC + 3;
			}
			break;
		case 0x9A: // TXS (Implied) X -> S
			{
				// Transfer index X to Stack Pointer
				state->SP = (uint8_t)state->X; // LSB
				state->PC = state->PC + 1;
			}
			break;
		case 0x9B: UnimplementedInstruction(); break;
		case 0x9C: UnimplementedInstruction(); break;
		case 0x9D: // STA $FFFF,X (Absolute,X) A -> M
			{
				tABSOLUTEX(opcode1, opcode2) = (uint8_t) state->A; // LSB
				state->PC = state->PC + 3;
			}
			break;
		case 0x9E: UnimplementedInstruction(); break;
		case 0x9F: UnimplementedInstruction(); break;

		case 0xA0: // LDY #$FF (Immediate) M -> Y
			{
				_ldy(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0xA1: // LDA ($FF,X) (Indexed Indirect,X)	M -> A
			{
				_lda(INDIRECTX(opcode1), 2);
			}
			break;
		case 0xA2: // LDX #$FF (Immediate) M -> X
			{
				_ldx(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0xA3: UnimplementedInstruction(); break;
		case 0xA4: // LDY $FF (Zeropage) M -> Y
			{
				_ldx(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0xA5: // LDA $FF (Zeropage) M -> A
			{
				_lda(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0xA6: // LDX $FF (ZeroPage) M -> X
			{
				_ldx(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0xA7: UnimplementedInstruction(); break;
		case 0xA8: // TAY (Implied) A -> Y
			{
				uint16_t answer = (uint16_t) state->A;
				StatusRegisterNegative(answer);
				StatusRegisterZero(answer);
				state->Y = (uint8_t) answer; // LSB
				state->PC = state->PC + 1;
			}
			break;
		case 0xA9: // LDA #$FF (Immediate) M -> A
			{
				_lda(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0xAA: // TAX (Implied) A -> X
			{
				uint8_t answer = (uint8_t) state->A;
				StatusRegisterNegative(answer);
				StatusRegisterZero(answer);
				state->X = (uint8_t) answer; // LSB
				state->PC = state->PC + 1;
			}
			break;
		case 0xAB: UnimplementedInstruction(); break;
		case 0xAC: // LDY $FFFF (Absolute) M -> Y
			{
				_ldy(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0xAD: // LDA $FFFF (Absolute) M -> A
			{
				_lda(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0xAE: // LDX $FFFF (Absolute) M -> X
			{
				_ldx(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0xAF: UnimplementedInstruction(); break;

		case 0xB0: // BCS $FFFF (Relative) Branch on C = 1
			{
				if (state->sr.C == 1)
					state->PC = BRANCH(opcode1);
				else
					state->PC = state->PC + 2;
			}
			break;
		case 0xB1: // LDA ($FF),Y (Indirect Indexed),Y	M -> A
			{
				_lda(INDIRECTY(opcode1), 2);
			}
			break;
		case 0xB2: UnimplementedInstruction(); break;
		case 0xB3: UnimplementedInstruction(); break;
		case 0xB4: // LDY $FF,X (Zeropage,X) M -> Y
			{
				_ldy(ZEROPAGEX(opcode1), 2);
			}
			break;
		case 0xB5: // LDA $FF,X (Zeropage,X) M -> A
			{
				_lda(ZEROPAGEX(opcode1), 2);
			}
			break;
		case 0xB6: // LDX $FF,Y (ZeroPage,Y) M -> X
			{
				_ldx(ZEROPAGEY(opcode1), 2);
			}
			break;
		case 0xB7: UnimplementedInstruction(); break;
		case 0xB8: // CLV (Implied) 0 -> V
			{
				state->sr.V = 0;
				state->PC = state->PC + 1;
			}
			break;
		case 0xB9: // LDA $FFFF,Y (Absolute,Y) M -> A
			{
				_lda(ABSOLUTEY(opcode1, opcode2), 3);
			}
			break;
		case 0xBA: // TSX (Implied) S -> X
			{
				// Transfer Stack Pointer to index X
				uint16_t answer = (uint16_t) state->SP;
				StatusRegisterNegative(answer);
				StatusRegisterZero(answer);
				state->X = (uint8_t) answer; // LSB
				state->PC = state->PC + 1;
			}
			break;
		case 0xBB: UnimplementedInstruction(state); break;
		case 0xBC: // LDY $FFFF,X (Absolute,X) M -> Y
			{
				_ldy(ABSOLUTEX(opcode1, opcode2), 3);
			}
			break;
		case 0xBD: // LDA $FFFF,X (Absolute,X) M -> A
			{
				_lda(ABSOLUTEX(opcode1, opcode2), 3);
			}
			break;
		case 0xBE: // LDX $FFFF,Y (Absolute,Y) M -> X
			{
				_ldx(ABSOLUTEY(opcode1, opcode2), 3);
			}
			break;
		case 0xBF: UnimplementedInstruction(); break;

		case 0xC0: // CPY #$FF (Immediate) Y - M
			{
				_cpy(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0xC1: // CMP ($FF,X) (Indirect,X) A - M
			{
				_cmp(INDIRECTX(opcode1), 2);
			}
			break;
		case 0xC2: UnimplementedInstruction(); break;
		case 0xC3: UnimplementedInstruction(); break;
		case 0xC4: // CPY $FF (Zeropage) Y - M
			{
				_cpy(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0xC5: // CMP $FF (Zeropage) A - M
			{
				_cmp(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0xC6: // DEC (ZeroPage) M - 1 -> M
			{
				_dec(ZEROPAGE(opcode1), 2, tZEROPAGE(opcode1));
			}
			break;
		case 0xC7: UnimplementedInstruction(); break;
		case 0xC8: // INY (Implied) Y + 1 -> Y
			{
				_iny();
			}
			break;
		case 0xC9: // CMP #$FF (Immediate) A - M
			{
				_cmp(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0xCA: // DEX (Implied) X - 1 -> X
			{
				_dex();
			}
			break;
		case 0xCB: UnimplementedInstruction(); break;
		case 0xCC: // CPY $FFFF (Absolute) Y - M
			{
				_cpy(ABSOLUTEY(opcode1, opcode2), 3);
			}
			break;
		case 0xCD: // CMP $FFFF (Absolute) A - M
			{
				_cmp(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0xCE: // DEC (Absolute) M - 1 -> M
			{
				_dec(ABSOLUTE(opcode1, opcode2), 3, tABSOLUTE(opcode1, opcode2));
			}
			break;
		case 0xCF: UnimplementedInstruction(); break;

		case 0xD0: // BNE $FFFF (Relative) Branch on Z = 0
			{
				if (state->sr.Z == 0)
					state->PC = BRANCH(opcode1);
				else
					state->PC = state->PC + 2;
			}
			break;
		case 0xD1: // CMP ($FF),Y (Indirect),Y A - M
			{
				_cmp(INDIRECTY(opcode1), 2);
			}
			break;
		case 0xD2: UnimplementedInstruction(); break;
		case 0xD3: UnimplementedInstruction(); break;
		case 0xD4: UnimplementedInstruction(); break;
		case 0xD5: // CMP $FF,X (ZeroPage,X) A - M
			{
				_cmp(ZEROPAGEX(opcode1), 2);
			}
			break;
		case 0xD6: // DEC (ZeroPage,X) M - 1 -> M
			{
				_dec(ZEROPAGEX(opcode1), 2, tZEROPAGEX(opcode1));
			}
			break;
		case 0xD7: UnimplementedInstruction(); break;
		case 0xD8: // CLD (Implied) 0 -> D
			{
				state->sr.D = 0;
				state->PC = state->PC + 1;
			}
			break;
		case 0xD9: // CMP $FFFF,Y (Absolute,Y) A - M
			{
				_cmp(ABSOLUTEY(opcode1, opcode2), 3);
			}
			break;
		case 0xDA: UnimplementedInstruction(); break;
		case 0xDB: UnimplementedInstruction(); break;
		case 0xDC: UnimplementedInstruction(); break;
		case 0xDD: // CMP $FFFF,X (Absolute,X) A - M
			{
				_cmp(ABSOLUTEX(opcode1, opcode2), 3);
			}
			break;
		case 0xDE: // DEC (Absolute,X) M - 1 -> M
			{
				_dec(ABSOLUTEX(opcode1, opcode2), 3, tABSOLUTEX(opcode1, opcode2));
			}
			break;
		case 0xDF: UnimplementedInstruction(); break;

		case 0xE0: // CPX #$FF (Immediate) X - M
			{
				_cpx(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0xE1: // SBC ($FF,X) (Indirect,X) A - M + ~C -> A
			{
				_sbc(INDIRECTX(opcode1), 2);
			}
			break;
		case 0xE2: UnimplementedInstruction(); break;
		case 0xE3: UnimplementedInstruction(); break;
		case 0xE4: // CPX $FF (Zeropage) X - M
			{
				_cpx(ZEROPAGEX(opcode1), 2);
			}
			break;
		case 0xE5: // SBC $FF (ZeroPage) A - M + ~C -> A
			{
				_sbc(ZEROPAGE(opcode1), 2);
			}
			break;
		case 0xE6: // INC (ZeroPage) M + 1 -> M
			{
				_inc(ZEROPAGE(opcode1), 2, tZEROPAGE(opcode1));
			}
			break;
		case 0xE7: UnimplementedInstruction(); break;
		case 0xE8: // INX (Implied) X + 1 -> X
			{
				_inx();
			}
			break;
		case 0xE9: // SBC #$FF (Immediate) A - M + ~C -> A
			{
				_sbc(IMMEDIATE(opcode1), 2);
			}
			break;
		case 0xEA: // NOP (Implied)
			{
				state->PC = state->PC + 1;
			}
			break;
		case 0xEB: UnimplementedInstruction(); break;
		case 0xEC: // CPX $FFFF (Absolute) X - M
			{
				_cpx(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0xED: // SBC $FFFF (Absolute) A - M + ~C -> A
			{
				_sbc(ABSOLUTE(opcode1, opcode2), 3);
			}
			break;
		case 0xEE: // INC (Absolute) M + 1 -> M
			{
				_inc(ABSOLUTE(opcode1, opcode2), 3, tABSOLUTE(opcode1, opcode2));
			}
			break;
		case 0xEF: UnimplementedInstruction(); break;

		case 0xF0: // BEQ $FFFF (Relative) Branch on Z = 1
			{
				if (state->sr.Z == 1)
					state->PC = BRANCH(opcode1);
				else
					state->PC = state->PC + 2;
			}
			break;
		case 0xF1: // SBC ($FF),Y ((Indirect),Y) A - M + ~C -> A
			{
				_sbc(INDIRECTY(opcode1), 2);
			}
			break;
		case 0xF2: UnimplementedInstruction(); break;
		case 0xF3: UnimplementedInstruction(); break;
		case 0xF4: UnimplementedInstruction(); break;
		case 0xF5: // SBC $FF,X (ZeroPage,X) A - M + ~C -> A
			{
				_sbc(ZEROPAGEX(opcode1), 2);
			}
			break;
		case 0xF6: // INC (ZeroPage,X) M + 1 -> M
			{
				_inc(ZEROPAGEX(opcode1), 2, tZEROPAGEX(opcode1));
			}
			break;
		case 0xF7: UnimplementedInstruction(); break;
		case 0xF8: // SED (Implied) 1 -> D
			{
				state->sr.D = 1;
				state->PC = state->PC + 1;
			}
			break;
		case 0xF9: // SBC $FFFF,Y (Absolute,Y) A - M + ~C -> A
			{
				_sbc(ABSOLUTEY(opcode1, opcode2), 3);
			}
			break;
		case 0xFA: UnimplementedInstruction(); break;
		case 0xFB: UnimplementedInstruction(); break;
		case 0xFC: UnimplementedInstruction(); break;
		case 0xFD: // SBC $FFFF,X (Absolute,X) A - M + ~C -> A
			{
				_sbc(ABSOLUTEX(opcode1, opcode2), 3);
			}
			break;
		case 0xFE: // INC (Absolute,X) M + 1 -> M
			{
				_inc(ABSOLUTEX(opcode1, opcode2), 3, tABSOLUTEX(opcode1, opcode2));
			}
			break;
		case 0xFF: UnimplementedInstruction(); break;
		default: break;
	}
	//printf("\t");
	//printf("%c", state->sr.N ? 'N' : 'n');
	//printf("%c", state->sr.V ? 'V' : 'v');
	//printf("%c", state->sr.dc ? '_' : '0');
	//printf("%c", state->sr.B ? 'B' : 'b');
	//printf("%c", state->sr.D ? 'D' : 'd');
	//printf("%c", state->sr.I ? 'I' : 'i');
	//printf("%c", state->sr.Z ? 'Z' : 'z');
	//printf("%c ", state->sr.C ? 'C' : 'c');
	//printf("A=$%02X,X=$%02X,Y=$%02X,SP=$%04X,SR=$%02X,PC=$%04X\n", state->A, state->X, state->Y, state->SP, state->sr, state->PC);
	return 0;
}

/*****************************************************************************
  Load the following Commodore C64 ROM:
    BASIC.ROM
	KERNAL.ROM
	CHAR.ROM

  Returns error if a ROM is not found.
******************************************************************************/
uint8_t C64_LoadROM()
{
	FILE *f;

	if ((f=fopen("./rom/basic.rom", "rb")) != NULL)
	{
		fread(pBasicROM, 8192, 1, f);
		fclose(f);
	}
	else
	{
		printf("error: Couldn't open BASIC.ROM\n");
		return 1;
	}

	if ((f=fopen("./rom/kernal.rom", "rb")) != NULL)
	{
		fread(pKernalROM, 8192, 1, f);
		fclose(f);
	}
	else
	{
		printf("error: Couldn't open KERNAL.ROM\n");
		return 1;
	}

	if ((f=fopen("./rom/char.rom", "rb")) != NULL)
	{
		fread(pCharROM, 4096, 1, f);
		fclose(f);
	}
	else
	{
		printf("error: Couldn't open CHAR.ROM\n");
		return 1;
	}

	return 0;
}

void ReadFileIntoMemoryAt(State6510* state, char* filename, uint16_t offset)
{
	FILE *f;
	int fsize;
	uint8_t *buffer;
//	uint8_t *new_buffer;

	f = fopen(filename, "rb");

	if (f==NULL)
	{
		printf("error: Couldn't open %s\n", filename);
		exit(1);
	}
	fseek(f, 0L, SEEK_END);
	fsize = ftell(f);
	fseek(f, 0L, SEEK_SET);
	
	buffer = (uint8_t*)malloc(fsize);
//	new_buffer = (uint8_t*)malloc(fsize - 2);

	fread(buffer, fsize, 1, f);
//	offset = (buffer[0] | (buffer[1] << 8));
//	state->PC = offset;

//	strncpy(new_buffer, buffer + 2, fsize - 2);
	memcpy(state->memory + offset, buffer, fsize);

	fclose(f);

	// Remove first two bytes from file (start address which you can use as the offset for memcpy)
	// when copying to state->memory add one address to the base.
}

/*
  Allocates memory for the ROM files
*/
uint8_t C64_AllocateMemory()
{
	state = (State6510*)calloc(1, sizeof(State6510));
	pBasicROM = (uint8_t*)malloc(8192);
	pKernalROM = (uint8_t*)malloc(8192);
	pCharROM = (uint8_t*)malloc(4096);

	return (pBasicROM != NULL && pKernalROM != NULL && pCharROM != NULL);
}

/*****************************************************************************
*** INITIALIZE THE 6510                                                   ***
*****************************************************************************/
void Init6510(void)
{
	// Initialize state memory
	state->memory = (uint8_t*)malloc(0x40000);  //64K
	// Clear memory
	for (int i = 0; i <= 65535; i++)
		state->memory[i] = 0;
	// after a start page 127 MOS programming manual
	// 8 cycles, fetch vector from 0xFFFC and 0xFFFD
	state->A = 0;
	state->X = 0;
	state->Y = 0;
	state->sr.C = 0;
	state->sr.Z = 0;
	state->sr.I = 0;
	state->sr.D = 0;
	state->sr.B = 1;
	state->sr.dc = 1;
	state->sr.V = 0;
	state->sr.N = 0;
	state->SP = 0x01ff; // points to end of the stack in memory $01FF.
	
	//state->PC = 0xE000;	//state->PC = 0xFCE2; // First time startup vector
	state->PC = 0x080e;

	state->memory[0x0000] = 0x2f; // Data Direction Register
	state->memory[0x0001] = 0x37; // Data Register

	state->memory[0xFFFA] = 0x43; // 0xFE43
	state->memory[0xFFFB] = 0xfe;
	state->memory[0xFFFC] = 0xe2; // 0xFCE2
	state->memory[0xFFFD] = 0xfc;
	state->memory[0xFFFE] = 0x48; // 0xFF48
	state->memory[0xFFFF] = 0xff;

	//
	// Test routines to test the emulator
	//
	//ReadFileIntoMemoryAt(state, "./test_files/decimal_test/decimal_test.prg", 0x0801); // test software
	// in state->memory[0x09be] = the variable that holds the outcome: 0 = succes, 1 = error
	ReadFileIntoMemoryAt(state, "./test_files/overflow_test/overflow_test.prg", 0x0801); // test software
	// in state->memory[0x0896] = the variable that holds the outcome: 0 = succes, 1 = error

	//ReadFileIntoMemoryAt(state, "./test_files/asl_Compiled.prg", 0x0801); // test software
}

uint8_t main (int argc, char**argv)
{
	int done = 0;
	//clock_t t1, lastinterrupt;

	//t1 = clock();
	//lastinterrupt = clock();
	
	if (!C64_AllocateMemory()) return 1;
	if (C64_LoadROM()) return 1;
	Init6510();


	while (done == 0)
	{
		done = Emulate6510Op(state);
		//if (clock() - lastinterrupt > 1.0/60.0) // 1/60 second has elapsed
		//{
		//	printf("1 cycle\n");
		//	lastinterrupt = clock();
		//}
	}

	return 0;
}
