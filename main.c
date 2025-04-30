#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEBUG

#ifdef DEBUG
    #define DEBUG_PRINT(...) do { printf(__VA_ARGS__); } while(0)
#else
    #define DEBUG_PRINT(...) do { } while(0)
#endif

const char line_buf[] = "-----------------------------------------------------------------------";
const char space_buf[] = "                                                                       ";

typedef char bool;
#define TRUE 1
#define FALSE 0

#define MEM_BASE 0x80
#define MEM_SIZE 16
#define MEM_VAL_BASE 0x40

#define INIT_ZERO(NAME) memset(NAME, 0, sizeof(NAME))

int mem[MEM_SIZE];

#define MAX_COMMIT 2

struct reorder_entry {
    bool busy;
    bool issued;
    bool finished;
    int instr_addr;
    int tag;
    bool s;
};

#define MAX_ROB_SIZE 8

struct reorder_entry rob[MAX_ROB_SIZE];
int rob_head = 0, rob_tail = 0, rob_size = 0;
bool should_flush = FALSE;
int fault_ia = 0;

#define REG_FILE_SIZE 12
#define REG_VAL_BASE 0x40

struct reg_file_entry {
    int reg;
    int data;
    bool valid;
    bool busy;
};

struct reg_file_entry reg_file[REG_FILE_SIZE];

#define RAT_SIZE 8

struct rat_entry
{
   int tag;
};

struct rat_entry front_rat[RAT_SIZE], back_rat[RAT_SIZE];

struct pipeline_entry {
    int instr_addr;
    int tag;
    int val;
    int addr;
    int rob_i;
};

#define ALU_PIPELINE_SIZE 1
#define LOAD_PIPELINE_SIZE 1
#define STORE_PIPELINE_SIZE 1
#define STORE_BUF_SIZE 4
#define COMPLETED_LOAD_BUF_SIZE 4

struct pipeline_entry alu_pipeline[ALU_PIPELINE_SIZE];
struct pipeline_entry load_pipeline[LOAD_PIPELINE_SIZE];
struct pipeline_entry store_pipeline[STORE_PIPELINE_SIZE];
struct pipeline_entry store_buffer[STORE_BUF_SIZE];
struct pipeline_entry completed_load_buffer[COMPLETED_LOAD_BUF_SIZE];

int finished_store = 0;
int committed_store = 0;
int completed_load = 0;

enum instr_type {
    ADD,
    SUB,
    AND,
    OR,
    XOR,
    NOT,
    LOAD,
    STORE,
    NOP,
};

struct instr_window_entry {
    int instr_addr;
    int out_tag;
    int instr;
    int op1;
    int op2;
    int off;
    bool busy;
    bool ready;
    int rob_i;
};

#define INSTR_WINDOW_SIZE 4

struct instr_window_entry instr_window[INSTR_WINDOW_SIZE];
int instr_window_num = 0;

struct instr {
    int type;
    int dst;
    int src1;
    int src2;
    int offset;
};

#define MAX_INSTR_NUM 32

struct instr icache[MAX_INSTR_NUM + 1];
int instr_num = 0;
int pc = 1;

char* type_to_str(int type) {
    switch (type) {
        case LOAD: {
            return "lb";
        }
        case STORE: {
            return "sb";
        }
        case ADD: {
            return "add";
        }
        case SUB: {
            return "sub";
        }
        case AND: {
            return "and";
        }
        case OR: {
            return "or";
        }
        default: return "";
    }
}

#define REMOVE_ENTRY(buf, idx, buf_size) do { \
    for(int _i = idx; _i < buf_size - 1; _i++) { \
        buf[_i] = buf[_i + 1];                \
    }                                       \
} while(0) 

void invalidate_pipeline_entry(struct pipeline_entry *e) {
    e->addr = 0;
    e->instr_addr = -1;
    e->rob_i = -1;
    e->tag = -1;
    e->val = 0;
}

void init() {
    INIT_ZERO(mem);
    INIT_ZERO(icache);
    INIT_ZERO(reg_file);
    INIT_ZERO(rob);
    INIT_ZERO(front_rat);
    INIT_ZERO(back_rat);
    INIT_ZERO(alu_pipeline);
    INIT_ZERO(store_pipeline);
    INIT_ZERO(load_pipeline);
    INIT_ZERO(store_buffer);
    INIT_ZERO(completed_load_buffer);

    for(int i = 0; i < MEM_SIZE; i++) {
        mem[i] = MEM_VAL_BASE + i;
    }

    for(int i = 0; i < ALU_PIPELINE_SIZE; i++) {
        alu_pipeline[i].instr_addr = -1;
        alu_pipeline[i].tag = -1;
        store_pipeline[i].instr_addr = -1;
        store_pipeline[i].tag = -1;
        load_pipeline[i].instr_addr = -1;
        load_pipeline[i].tag = -1;
    }

    for(int i = 0; i < REG_FILE_SIZE; i++){
        reg_file[i].data = REG_VAL_BASE + i;
        if (i < RAT_SIZE) {
            front_rat[i].tag = i;
            back_rat[i].tag = i;
            reg_file[i].reg = i;
            reg_file[i].busy = TRUE;
            reg_file[i].valid = TRUE;
        } else {
            reg_file[i].reg = -1;
        }  
    }
}

void flush() {
    printf("[Flush] starting flush IA %d\n", fault_ia);
    for (int i = 0; i < MAX_ROB_SIZE; i++) {
        if (rob[i].busy && rob[i].s) {
            reg_file[rob[i].tag].busy = FALSE;
            reg_file[rob[i].tag].valid = FALSE;
            reg_file[rob[i].tag].reg = -1;
            rob[i].busy = FALSE;
            rob_size--;

            if (rob[i].instr_addr == fault_ia)
                rob_tail = i;

            if (rob_size == 0)
                rob_head = rob_tail = 0;
        }
    }

    for (int i = 0; i < completed_load; i++) {
        if (completed_load_buffer[i].instr_addr > fault_ia) {
            REMOVE_ENTRY(completed_load_buffer, i, completed_load);
            invalidate_pipeline_entry(&completed_load_buffer[completed_load - 1]);
            completed_load--;
        }
    }

    for (int i = 0; i < finished_store; i++) {
        if (store_buffer[i].instr_addr > fault_ia) {
            REMOVE_ENTRY(store_buffer, i, finished_store);
            invalidate_pipeline_entry(&store_buffer[finished_store - 1]);
            finished_store--;
        }
    }

    for (int i = 0; i < ALU_PIPELINE_SIZE; i++) {
        if (alu_pipeline[i].instr_addr > fault_ia)
            invalidate_pipeline_entry(&alu_pipeline[i]);
    }

    for (int i = 0; i < STORE_PIPELINE_SIZE; i++) {
        if (store_pipeline[i].instr_addr > fault_ia)
            invalidate_pipeline_entry(&store_pipeline[i]);
    }

    for (int i = 0; i < LOAD_PIPELINE_SIZE; i++) {
        if (load_pipeline[i].instr_addr > fault_ia)
            invalidate_pipeline_entry(&load_pipeline[i]);
    }

    for (int i = 0; i < instr_window_num; i++) {
        if (instr_window[i].instr_addr > fault_ia) {
            REMOVE_ENTRY(instr_window, i, instr_window_num);
            instr_window[instr_window_num - 1].busy = FALSE;
            instr_window[instr_window_num - 1].instr_addr = -1;
            instr_window_num--;
        }
    }

    for (int i = 0; i < RAT_SIZE; i++) {
        front_rat[i].tag = back_rat[i].tag;
    }

    pc = fault_ia;
    fault_ia = 0;
}

void store_mem() {
    for(int i = 0; i < committed_store; i++) {
        int addr = store_buffer[finished_store + i].addr;
        int val = store_buffer[finished_store + i].val;

        invalidate_pipeline_entry(&store_buffer[finished_store + i]);

        mem[addr] = val;
    }
    committed_store = 0;
}

void mark_speculative(int instr_addr) {
    bool start = FALSE;
    for(int n = 0, i = rob_head; n < rob_size; n++) {
        if (rob[i].busy && rob[i].instr_addr == instr_addr) {
            if (rob[i].s)
                return;
            start = TRUE;
        }

        if (start)
            rob[i].s = TRUE;

        i = (i + 1) % MAX_ROB_SIZE;
    }

    fault_ia = instr_addr;
}

void commit_store_or_load(int instr_addr) {
    for(int i = 0; i < finished_store; i++) {
        if(store_buffer[i].instr_addr == instr_addr) {
            struct pipeline_entry e = store_buffer[i];
            REMOVE_ENTRY(store_buffer, i, finished_store + committed_store);
            finished_store--;
            store_buffer[finished_store + committed_store++] = e;
            for(int j = 0; j < completed_load; j++) {
                if (completed_load_buffer[i].addr == e.addr && completed_load_buffer[i].instr_addr > e.instr_addr) {
                    should_flush = TRUE;
                    mark_speculative(completed_load_buffer[i].instr_addr);
                }
            }
            break;
        }
    }

    for(int i = 0; i < completed_load; i++) {
        if (completed_load_buffer[i].instr_addr == instr_addr) {
            invalidate_pipeline_entry(&completed_load_buffer[i]);
            break;
        }
    }
}

void retire() {
    store_mem();

    for(int i = 0; i < MAX_COMMIT && rob[rob_head].finished && rob[rob_head].busy && !rob[rob_head].s; i++) {
        int target_reg = reg_file[rob[rob_head].tag].reg;
        int evicted_tag = back_rat[target_reg].tag;

        printf("[Retire] retire ROB IA %d\n", rob[rob_head].instr_addr);
        printf("[Retire] Back End RAT $%d with new tag %d\n", target_reg, rob[rob_head].tag);
        printf("[Retire] invalidate Physical Register File $%d\n", evicted_tag);

        back_rat[target_reg].tag = rob[rob_head].tag;
        reg_file[evicted_tag].busy = FALSE;
        reg_file[evicted_tag].valid = FALSE;
        rob[rob_head].busy = FALSE;
        commit_store_or_load(rob[rob_head].instr_addr);

        rob_head = (rob_head + 1) % MAX_ROB_SIZE;
        rob_size--;
    }
}

void complete_alu() {
    for (int i = 0; i < ALU_PIPELINE_SIZE; i++) {
        if(alu_pipeline[i].instr_addr == -1)
            continue;

        printf("[Complete] validate Register File $%d with result %#x\n", alu_pipeline[i].tag, alu_pipeline[i].val);
        //Update Reg File
        reg_file[alu_pipeline[i].tag].valid = TRUE;
        reg_file[alu_pipeline[i].tag].data = alu_pipeline[i].val;
        
        //Update ROB
        printf("[Complete] update ROB IA %d finished\n", alu_pipeline[i].instr_addr);
        rob[alu_pipeline[i].rob_i].finished = TRUE;

        invalidate_pipeline_entry(&alu_pipeline[i]);
    }
}

void complete_mem() {
    for(int i = 0; i < STORE_PIPELINE_SIZE && finished_store + committed_store < COMPLETED_LOAD_BUF_SIZE; i++) {
        if(store_pipeline[i].instr_addr == -1)
            continue;
        printf("[Complete] Update ROB IA %d finished\n", store_pipeline[i].instr_addr);
        rob[store_pipeline[i].rob_i].finished = TRUE;

        store_buffer[finished_store++] = store_pipeline[i];
        invalidate_pipeline_entry(&store_pipeline[i]);       
    }
    for(int i = 0; i < LOAD_PIPELINE_SIZE && completed_load < COMPLETED_LOAD_BUF_SIZE; i++) {
        if(load_pipeline[i].instr_addr == -1)
            continue;
        printf("[Complete] validate Register File $%d with result 0x%x\n", load_pipeline[i].tag, load_pipeline[i].val);
        //Update Reg File
        reg_file[load_pipeline[i].tag].valid = TRUE;
        reg_file[load_pipeline[i].tag].data = load_pipeline[i].val;
        
        //Update ROB
        printf("[Complete] Update ROB IA %d finished\n", load_pipeline[i].instr_addr);
        rob[load_pipeline[i].rob_i].finished = TRUE;

        completed_load_buffer[completed_load++] = load_pipeline[i];
        invalidate_pipeline_entry(&load_pipeline[i]);
    }
}

void complete() {
    complete_alu();
    complete_mem();
}

void issue_alu(int i){
    int op1 = reg_file[instr_window[i].op1].data;
    int op2 = reg_file[instr_window[i].op2].data;
    int result = 0;

    switch (instr_window[i].instr) {
        case ADD: {
            result = op1 + op2;
        } break;
        case SUB: {
            result = op1 - op2;
        } break;
        case AND: {
            result = op1 & op2;
        } break;
        case OR: {
            result = op1 | op2;
        } break;
        default: break;
    }

    for(int j = 0; j < ALU_PIPELINE_SIZE; j++) {
        if(alu_pipeline[j].instr_addr == -1) {
            alu_pipeline[j].instr_addr = instr_window[i].instr_addr;
            alu_pipeline[j].val = result;
            alu_pipeline[j].tag = instr_window[i].out_tag;
            alu_pipeline[j].rob_i = instr_window[i].rob_i;
            printf("[Issue] issue IA %d to ALU %d with result 0x%x\n", instr_window[i].instr_addr, j, result);
            break;
        }
    }
}

int load_forward(int addr) {
    for(int i = 0; i < STORE_BUF_SIZE; i++) {
        if (store_buffer[i].addr == addr) {
            printf("[Issue] forward address %#x with value %#x\n", addr, store_buffer[i].val);
            return store_buffer[i].val;
        }
    }
    printf("[Issue] load memory %#x with value %#x\n", addr, mem[addr]);
    return mem[addr - MEM_BASE];
}

void issue_mem(int i){
    if (instr_window[i].instr == STORE) {
        for(int j = 0; j < STORE_PIPELINE_SIZE; j++) {
            if(store_pipeline[j].instr_addr == -1) {
                printf("[Issue] issue IA %d to store pipeline %d\n", instr_window[i].instr_addr, j);
                store_pipeline[j].instr_addr = instr_window[i].instr_addr;
                store_pipeline[j].val = reg_file[instr_window[i].op1].data;
                store_pipeline[j].addr = reg_file[instr_window[i].op2].data + instr_window[i].off;
                store_pipeline[j].tag = -1;
                store_pipeline[j].rob_i = instr_window[i].rob_i;
                break;
            }
        }
    } else {
        for(int j = 0; j < LOAD_PIPELINE_SIZE; j++) {
            if(load_pipeline[j].instr_addr == -1) {
                printf("[Issue] issue IA %d to load pipeline %d\n", instr_window[i].instr_addr, j);
                load_pipeline[j].instr_addr = instr_window[i].instr_addr;
                load_pipeline[j].addr = reg_file[instr_window[i].op2].data + instr_window[i].off;
                load_pipeline[j].val = load_forward(load_pipeline[j].addr);
                load_pipeline[j].tag = instr_window[i].out_tag;
                load_pipeline[j].rob_i = instr_window[i].rob_i;
                break;
            }
        }
    }
}

bool is_instr_ready(int i) {
    bool op1_ready = reg_file[instr_window[i].op1].valid;
    bool op2_ready = reg_file[instr_window[i].op2].valid;

    if (instr_window[i].ready)
        return TRUE;

    switch (instr_window[i].instr) {
        case LOAD:
        case STORE: {
            return op2_ready;
        } break;
        default: {
            return op1_ready && op2_ready;
        } break;
    }
}

void issue() {
    int issued_alu = 0, issued_store = 0, issued_load = 0;

    for (int i = 0; i < instr_window_num; i++) {
        bool old_ready = instr_window[i].ready;
        instr_window[i].ready = is_instr_ready(i);

        if (instr_window[i].ready) {
            if (!old_ready)
                printf("[Issue] IA %d get ready\n", instr_window[i].instr_addr);

            rob[instr_window[i].rob_i].issued = TRUE;
            
            if(instr_window[i].instr == STORE && issued_store < STORE_PIPELINE_SIZE) {
                issued_store++;
                issue_mem(i);
            } else if (instr_window[i].instr == LOAD && issued_load < LOAD_PIPELINE_SIZE) {
                issued_load++;
                issue_mem(i);
            } else if (issued_alu < ALU_PIPELINE_SIZE){
                issued_alu++;
                issue_alu(i);
            } else {
                DEBUG_PRINT("[Debug] pipeline full for IA %d\n", i);
                rob[instr_window[i].rob_i].issued = FALSE;
            }
            // Assuem pipelines are always available
            if (rob[instr_window[i].rob_i].issued) {
                printf("[Issue] update ROB IA %d issued\n", instr_window[i].instr_addr);
                REMOVE_ENTRY(instr_window, i, instr_window_num);
                instr_window_num--;
                i--;  
            }
        }       
    }
}

int next_tag() {
    for(int i = 0; i < REG_FILE_SIZE; i++) {
        if (reg_file[i].valid == FALSE && reg_file[i].busy == FALSE)
            return i;
    }
    return -1;
}

void rename_dispatch() {
    if (instr_window_num == INSTR_WINDOW_SIZE) {
        DEBUG_PRINT("[Debug] instr window full\n");
        return;
    }

    if (pc > instr_num) {
        DEBUG_PRINT("[Debug] no more instr to execute\n");
        return;
    }

    if (rob_size == MAX_ROB_SIZE) {
        DEBUG_PRINT("[Debug] rob full\n");
        return;
    }

    int i = instr_window_num;
    struct instr instr = icache[pc];
    int tag = -1;

    instr_window[i].op1 = instr.type == LOAD ? -1 : front_rat[instr.src1].tag;
    instr_window[i].op2 = front_rat[instr.src2].tag;

    if (instr.type != STORE) {
        tag = next_tag();
        if(tag == -1) {
            DEBUG_PRINT("[Debug] reg file full\n");
            return;
        }
        printf("[Rename] allocate Tag %d to IA %d with Reg %d\n", tag, pc, instr.dst);
        front_rat[instr.dst].tag = tag;
        reg_file[tag].busy = TRUE;
        reg_file[tag].valid = FALSE;
        reg_file[tag].reg = instr.dst;
    }

    printf("[Dispatch] insert IA %d to instruction window\n", pc);

    instr_window[i].out_tag = tag;
    instr_window[i].off = instr.offset;
    instr_window[i].instr = instr.type;
    instr_window[i].instr_addr = pc;
    instr_window[i].busy = TRUE;
    instr_window[i].ready = FALSE;
    instr_window[i].rob_i = rob_tail;
    instr_window[i].ready = is_instr_ready(i);
    instr_window_num++;

    i = rob_tail;
    rob[i].busy = TRUE;
    rob[i].finished = FALSE;
    rob[i].issued = FALSE;
    rob[i].instr_addr = pc;
    rob[i].tag = tag;
    rob[i].s = FALSE;
    rob_tail = (rob_tail + 1) % MAX_ROB_SIZE;
    rob_size++;
    pc++;
}

int parse_opcode(char* opcode){
    if (strcmp(opcode, "lb") ==  0|| strcmp(opcode, "load") == 0) {
        return LOAD;
    } else if (strcmp(opcode, "sb") ==  0|| strcmp(opcode, "store") == 0) {
        return STORE;
    } else if (strcmp(opcode, "add") == 0) {
        return ADD;
    } else if (strcmp(opcode, "sub") == 0) {
        return SUB;
    } else if (strcmp(opcode, "and") == 0) {
        return AND;
    } else if (strcmp(opcode, "or") == 0){
        return OR;
    } else {
        return -1;
    }
}

void load_instrs() {
    printf("format: opcode $dst, $src1, $src2 | opcode $op1, offset($op2)\n");
    printf("number of instructions: ");
    scanf("%d", &instr_num);
    for(int i = 1; i <= instr_num; i++){
        printf("enter instruction: ");
        char opcode[6] = {};
        scanf("%s ", opcode);

        icache[i].type = parse_opcode(opcode);
        icache[i].dst = 0;
        icache[i].src1 = 0;
        icache[i].src2 = 0;
        icache[i].offset = 0;

        switch (icache[i].type) {
            case LOAD: {
                scanf("$%d, ", &icache[i].dst);
                scanf("%x($%d)", &icache[i].offset, &icache[i].src2);
            } break;
            case STORE: {
                scanf("$%d, ", &icache[i].src1);
                scanf("%x($%d)", &icache[i].offset, &icache[i].src2);
            } break;
            case ADD:
            case SUB:
            case AND:
            case OR: {
                scanf("$%d, $%d, $%d", &icache[i].dst, &icache[i].src1, &icache[i].src2);
            } break;

            default: printf("Invalid Opcode\n"); return;
        }
    }
    #ifdef DEBUG
    for(int i = 1; i <= instr_num; i++){
        switch (icache[i].type) {
            case LOAD: {
                printf("Load instr %s $%d, 0x%x($%d)\n", "ld", icache[i].dst, icache[i].offset, icache[i].src2);
            } break;
            case STORE: {               
                printf("Load instr %s $%d, 0x%x($%d)\n", "sb", icache[i].src1, icache[i].offset, icache[i].src2);
            } break;
            case ADD: case SUB: case AND: case OR: {
                printf("Load instr %s $%d, $%d, $%d\n", type_to_str(icache[i].type), icache[i].dst, icache[i].src1, icache[i].src2);
                
            } break;
            default: return;
        }
    }
    #endif
    putchar('\n');
}

void print_line(int n) {
    for(int i = 0; i < n; i++) {
        putchar('-');
    }
}

void print_divider(int n) {
    print_line(n);
    putchar('\n');
}

void print_space(int n) {
    for(int i = 0; i < n; i++) {
        putchar(' ');
    }
}

void print_reg_file() {
    putchar('\n');
    printf("[Physical Register File]\n");
    print_divider(21);
    printf("TAG|Reg|Data|  V|  B|\n");
    print_divider(21);
    for(int i = 0; i < REG_FILE_SIZE; i++) {
        struct reg_file_entry e = reg_file[i];
        printf("%3d|%3d|%4x|%3d|%3d|\n", i, e.reg, e.data, e.valid, e.busy);
        print_divider(21);
    }
}

void print_rat() {
    putchar('\n');
    print_space(4);
    printf("[Front End RAT]");
    print_space(8);
    printf("[Back End RAT]\n");
    print_divider(48);
    print_space(4);
    printf("REG|     TAG|");
    print_space(12);
    printf("REG|     TAG|\n");
    print_divider(48);
    for(int i = 0; i < RAT_SIZE; i++) {   
        print_space(4);
        printf("$%-2d|%8d|", i, front_rat[i].tag);
        print_space(12);
        printf("$%-2d|%8d|\n", i, back_rat[i].tag);
        print_divider(48);
    }
    putchar('\n');
}

#define PRINT_ROB_ROW(NAME, FIELD) do {                                           \
    printf("%s", NAME);                                                           \
    for(int n = 0, i = rob_head; n < rob_size; n++, i = (i + 1) % MAX_ROB_SIZE) { \
        printf("%2d|", rob[i].FIELD);                                             \
    }                                                                             \
    putchar('\n');                                                                \
    print_divider(32);                                                               \
} while(0)                                                                        \

void print_rob() {
    putchar('\n');
    print_space(14);
    printf("[ROB]\n");
    print_divider(32);

    PRINT_ROB_ROW("  B|", busy);
    PRINT_ROB_ROW("  I|", issued);
    PRINT_ROB_ROW("  F|", finished);
    PRINT_ROB_ROW(" IA|", instr_addr);
    PRINT_ROB_ROW("TAG|", tag);
    PRINT_ROB_ROW("  S|", s);

    putchar('\n');
}

void print_instr_window() {
    putchar('\n');
    print_space(5);
    printf("[Instruction Window]\n");
    print_divider(32);
    printf("IA|OutTag|Instr|OP1|OP2| Off|B|R|\n");
    print_divider(32);
    for(int i = 0; i < instr_window_num; i++) {
        struct instr_window_entry e = instr_window[i];
        printf("%2d|%6d|%5s|%3d|%3d|%#4x|%1d|%1d|\n", e.instr_addr, e.out_tag, type_to_str(e.instr), e.op1, e.op2, e.off, e.busy,e.ready);
        print_divider(32);
    }
    putchar('\n');
}

void print_pipeline() {
    putchar('\n');
    print_space(4);
    printf("[ALU Pipeline]");
    print_space(16);
    printf("[Store Pipeline]");
    print_space(16);
    printf("[Load Pipeline]\n");
    
    for(int i = 0; i < ALU_PIPELINE_SIZE; i++) {
        print_line(22);
        print_space(8);
        print_line(24);
        print_space(8);
        print_divider(28);


        printf("stage|IA|TAG|(Result)|");
        print_space(8);
        printf("|stage|IA|(Addr)|(Data)|");
        print_space(8);
        printf("|stage|IA|TAG|(Addr)|(Data)|\n");

        print_line(22);
        print_space(8);
        print_line(24);
        print_space(8);
        print_divider(28);

        struct pipeline_entry e = alu_pipeline[i];
        printf("%5d|%2d|%3d|%#8x|", 0, e.instr_addr, e.tag, e.val);
        print_space(8);
        e = store_pipeline[i];
        printf("|%5d|%2d|%#6x|%#6x|", 0, e.instr_addr, e.addr, e.val);
        print_space(8);
        e = load_pipeline[i];
        printf("|%5d|%2d|%3d|%#6x|%#6x|\n", 0, e.instr_addr, e.tag, e.addr, e.val);

        print_line(22);
        print_space(8);
        print_line(24);
        print_space(8);
        print_divider(28);

        putchar('\n');
    }
}

#define LINE(n) (line_buf + sizeof(line_buf) - n)
#define SPACE(n) (space_buf + sizeof(space_buf) - n)

void print_mem_and_buffer() {
    putchar('\n');

    printf("%s[Store Buffer]%s[Memory Contents]\n", SPACE(11), SPACE(8));
    printf("%s%s%s%s\n", SPACE(11), LINE(15), SPACE(8), LINE(16));

    printf("%s|IA|DATA|Addr|%s|Address|Value|\n", SPACE(11), SPACE(8));
    printf("%s%s%s%s\n", SPACE(11), LINE(15), SPACE(8), LINE(16));
    
    for(int i = 0; i < STORE_BUF_SIZE; i++) {
        printf("%s", i == finished_store ? "Committed " : i == 0 ? "Finished  " :  SPACE(11));
        printf("|%2d|%#4x|%4X|", store_buffer[i].instr_addr, store_buffer[i].val, store_buffer[i].addr);
        printf("%s", SPACE(8));
        
        if (i < MEM_SIZE) {
            printf("|%#7x|%#5x|", MEM_BASE + i, mem[i]);
        }
        printf("\n%s%s%s%s\n", SPACE(11), LINE(15), SPACE(8), LINE(16));
    }

    for(int i = 0; i < COMPLETED_LOAD_BUF_SIZE + 2; i++) {
        printf("%s", i == 0 ? SPACE(6) : SPACE(11));
        if (i == 0) {
            printf("[Completed Load Buffer]");
        } else if (i == 1) {
            printf("|IA|     Addr|");
        } else {
            printf("|%2d|%9X|", completed_load_buffer[i - 2].instr_addr, completed_load_buffer[i - 2].addr);
        }
        
        printf("%s", i == 0 ? SPACE(4) : SPACE(8));
        
        if (STORE_BUF_SIZE + i < MEM_SIZE) {
            printf("|%#7x|%#5x|", MEM_BASE + STORE_BUF_SIZE + i, mem[STORE_BUF_SIZE + i]);
        }
        printf("\n%s%s%s%s\n", SPACE(11), LINE(15), SPACE(8), LINE(16));
    }

    for(int i = STORE_BUF_SIZE + COMPLETED_LOAD_BUF_SIZE + 2; i < MEM_SIZE; i++) {
        printf("%s", SPACE(32));
        printf("|%#7x|%#5x|", MEM_BASE + i, mem[i]);
        printf("\n%s%s\n", SPACE(32), LINE(16));
    }

    putchar('\n');
}

void print_info() {
    print_instr_window();
    print_rob();
    print_rat();
    print_reg_file();
    print_pipeline();
    print_mem_and_buffer();
}

void simulate_cycle(bool delay_flush) {
    if (should_flush) {
        flush();
        should_flush = FALSE;
        return;
    }

    retire();

    if (!delay_flush && should_flush) {
        flush();
        should_flush = FALSE;
        return;
    }

    complete();
    issue();
    rename_dispatch();
    rename_dispatch();
}

int main() {
    init();
    load_instrs();
    printf("press enter to continue");
    getchar();
    getchar();
    int cycle = 0;
    bool delay_flush = FALSE;
    while(pc <= instr_num || rob_size != 0) {
        system("clear");
        system("clear");
        print_divider(64);
        printf("\n#CYCLE %d\n\n", cycle);
        simulate_cycle(delay_flush);
        print_info();
        printf("press enter to continue");
        while(getchar() != '\n');
        putchar('\n');
        cycle++;
    }
    print_divider(64);
    printf("End of Simulation\n");
}