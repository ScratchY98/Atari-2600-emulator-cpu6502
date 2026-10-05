#include <stdio.h>
#include <stdint.h>

#define ADDR_A0   (1 << 0)   // Register/offset select bit
#define ADDR_A1   (1 << 1)   // Register/offset select bit
#define ADDR_A2   (1 << 2)   // RIOT: 0=ports, 1=timer
#define ADDR_A3   (1 << 3)   // TIA read register bit
#define ADDR_A4   (1 << 4)   // RIOT: timer write select
#define ADDR_A5   (1 << 5)   // TIA write register bit
#define ADDR_A6   (1 << 6)   // RAM index bit (TIA ignores)
#define ADDR_A7   (1 << 7)   // 0=TIA, 1=RIOT
#define ADDR_A8   (1 << 8)   // Unused by console (mirrors)
#define ADDR_A9   (1 << 9)   // RIOT: 0=RAM, 1=I/O
#define ADDR_A10  (1 << 10)  // Cartridge offset only
#define ADDR_A11  (1 << 11)  // Cartridge offset only
#define ADDR_A12  (1 << 12)  // 1=cartridge, 0=TIA/RIOT

#define ADDR_MASK       0x1FFF  // 13-bit address bus
#define CART_MASK       0x0FFF  // 4K cartridge window offset
#define ROM_2K_MASK     0x07FF  // 2K ROM mirror mask
#define RAM_MASK        0x7F    // 128-byte RAM index
#define TIA_WRITE_MASK  0x3F    // 64 TIA write registers
#define TIA_READ_MASK   0x0F    // 16 TIA read registers

#define FLAG_C  (1 << 0)  // Carry
#define FLAG_Z  (1 << 1)  // Zero result
#define FLAG_I  (1 << 2)  // IRQ disable
#define FLAG_D  (1 << 3)  // Decimal mode (BCD)
#define FLAG_B  (1 << 4)  // Break (only when pushed)
#define FLAG_U  (1 << 5)  // Unused, always 1
#define FLAG_V  (1 << 6)  // Signed overflow
#define FLAG_N  (1 << 7)  // Negative (bit 7)

#define RESET_SP   0xFD               // SP after reset (0 - 3)

#define NMI_VEC    0xFFFA             // NMI vector address
#define RESET_VEC  0xFFFC             // Reset vector address
#define IRQ_VEC    0xFFFE             // IRQ/BRK vector address

typedef struct {
    uint8_t  A;
    uint8_t  X, Y;
    uint8_t  S;
    uint16_t PC;
    uint8_t  P;
} CPU;

uint8_t rom[32768];
size_t  rom_size;
uint8_t ram[128];      // RIOT RAM (128 bytes)

size_t load_rom(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        printf("Can't open ROM!\n");
        return 0;
    }
    size_t length = fread(rom, 1, sizeof rom, file);   // Whole file, from offset 0
    fclose(file);
    printf("ROM loaded: %zu bytes\n", length);
    return length;
}

uint8_t cart_read(uint16_t off)          // off: 0x000-0xFFF
{
    if (rom_size == 2048) return rom[off & ROM_2K_MASK];   // 2K mirrored twice
    return rom[off];                                        // 4K
}

uint8_t bus_read(uint16_t addr)
{
    addr &= ADDR_MASK;                                  // 13-bit address bus
    if (addr & ADDR_A12)    return cart_read(addr & CART_MASK);
    if (!(addr & ADDR_A7))  return 0;                   // TIA
    if (!(addr & ADDR_A9))  return ram[addr & RAM_MASK];   // RAM
    return 0;                                           // RIOT
}

void update_nz_flags(CPU *cpu, uint8_t value)
{
    cpu->P &= ~(FLAG_N | FLAG_Z);          // Clear N and Z
    cpu->P |= value & FLAG_N;              // N = bit 7
    cpu->P |= (value == 0) ? FLAG_Z : 0;   // Z if value=0
}

void conditional_jump(CPU *cpu, uint8_t reg_value, uint8_t request_value)
{
    if (reg_value == request_value) {
        int8_t offset = bus_read(cpu->PC + 1);
        cpu->PC += offset + 2;
    }
    else cpu->PC+=2;
}

int execute_opcode(CPU *cpu)
{
    uint8_t op = bus_read(cpu->PC);
    /*
    uint8_t cc  =  op       & 0b11;
    uint8_t bbb = (op >> 2) & 0b111;
    uint8_t aaa =  op >> 5;
    */

    //printf("PC=%04X OP=%02X  aaa=%d bbb=%d cc=%d\n", cpu->PC, op, aaa, bbb, cc);

    if ((op & 0b00011111) == 0b00010000) {
        uint8_t xx = (op & 0b11000000) >> 6;
        uint8_t y  = (op & 0b00100000) >> 5;
        
        switch (xx) {
            case 0b00: // N
                conditional_jump(cpu, (cpu->P & FLAG_N) != 0, y);
                break;
            case 0b01: // V
                conditional_jump(cpu, (cpu->P & FLAG_V) != 0, y);
                break;
            case 0b10: // C
                conditional_jump(cpu, (cpu->P & FLAG_C) != 0, y);
                break;
            case 0b11: // Z
                conditional_jump(cpu, (cpu->P & FLAG_Z) != 0, y);
                break;
        }

        return 1;
    }

    switch (op) {
        case 0x18: // CLC
            cpu->P &= ~FLAG_C;
            cpu->PC++;
            break;

        case 0x38: // SEC
            cpu->P |= FLAG_C;
            cpu->PC++;
            break;

        case 0x4C: // JMP abs
            cpu->PC = bus_read(cpu->PC + 1) | (bus_read(cpu->PC + 2) << 8);
            break;

        case 0x58: // CLI
            cpu->P &= ~FLAG_I;
            cpu->PC++;
            break;

        case 0x78: // SEI
            cpu->P |= FLAG_I;
            cpu->PC++;
            break;

        case 0x88: // DEY
            cpu->Y--;
            update_nz_flags(cpu, cpu->Y);
            cpu->PC++;
            break;

        case 0x8A: // TXA
            cpu->A = cpu->X;
            update_nz_flags(cpu, cpu->A);
            cpu->PC++;
            break;

        case 0x98: // TYA
            cpu->A = cpu->Y;
            update_nz_flags(cpu, cpu->A);
            cpu->PC++;
            break;

        case 0x9A: // TXS
            cpu->S = cpu->X;
            cpu->PC++;
            break;

        case 0xA8: // TAY
            cpu->Y = cpu->A;
            update_nz_flags(cpu, cpu->Y);
            cpu->PC++;
            break;

        case 0xAA: // TAX
            cpu->X = cpu->A;
            update_nz_flags(cpu, cpu->X);
            cpu->PC++;
            break;

        case 0xB8: // CLV
            cpu->P &= ~FLAG_V;
            cpu->PC++;
            break;

        case 0xBA: // TSX
            cpu->X = cpu->S;
            update_nz_flags(cpu, cpu->X);
            cpu->PC++;
            break;

        case 0xC8: // INY
            cpu->Y++;
            update_nz_flags(cpu, cpu->Y);
            cpu->PC++;
            break;

        case 0xCA: // DEX
            cpu->X--;
            update_nz_flags(cpu, cpu->X);
            cpu->PC++;
            break;

        case 0xD8: // CLD
            cpu->P &= ~FLAG_D;
            cpu->PC++;
            break;

        case 0xE8: // INX
            cpu->X++;
            update_nz_flags(cpu, cpu->X);
            cpu->PC++;
            break;

        case 0xEA: // NOP
            cpu->PC++;
            break;

        case 0xF8: // SED
            cpu->P |= FLAG_D;
            cpu->PC++;
            break;

        default:
            printf("UNEXPECTED OPCODE: %02X AT PC=%04X\n", op, cpu->PC);
            return 0;
    }

    return 1;
}

int main(void)
{
    CPU cpu = {0};

    rom_size = load_rom("breakout.a26");
    if (rom_size == 0) return 1;
    if (rom_size != 2048 && rom_size != 4096) {
        printf("Taille non geree pour l'instant : %zu\n", rom_size);
        return 1;
    }

    cpu.S  = RESET_SP;
    cpu.P  = FLAG_U | FLAG_I;
    cpu.PC = bus_read(RESET_VEC) | (bus_read(RESET_VEC + 1) << 8);
    printf("Reset vector : $%04X\n", cpu.PC);

    int running = 1;
    while (running) {
        running = execute_opcode(&cpu);
    }
    return 0;
}