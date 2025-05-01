#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef char bool;
#define TRUE 1
#define FALSE 0

// configurations
#define DELAY_FLUSH FALSE
#define MAX_COMMIT 2
#define MAX_RENAME_DISPATCH 2
#define MAX_INSTR_NUM 32

#ifdef DEFAULT_VALUE
    int default_reg[] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b};
    int default_mem[] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49};
    #define INIT_REG_VAL(i) (default_reg[i])
    #define INIT_MEM_VAL(i) (default_mem[i])
#else
    // increment values from the base
    #define MEM_VAL_BASE 0x40
    #define REG_VAL_BASE 0x40
    #define INIT_REG_VAL(i) (MEM_VAL_BASE + i)
    #define INIT_MEM_VAL(i) (MEM_VAL_BASE + i)
#endif

// default config for RAT version
#define MEM_BASE 0x80
#define MEM_SIZE 10
#define ROB_SIZE 8
#define REG_FILE_SIZE 12
#define RAT_SIZE 8
#define ALU_PORT_NUM 1
#define LOAD_PORT_NUM 1
#define STORE_PORT_NUM 1
#define STORE_BUF_SIZE 4
#define COMPLETED_LOAD_BUF_SIZE 4
#define INSTR_WINDOW_SIZE 4

#ifdef DEBUG
    #define DEBUG_PRINT(...) do { printf(__VA_ARGS__); } while(0)
#else
    #define DEBUG_PRINT(...) do { } while(0)
#endif                                                                      

#define INVALID_ENTRY(buf, idx) do {           \
    memset((buf) + (idx), 0, sizeof((buf)[0]));\
} while(0)                                     \

#define REMOVE_ENTRY(buf, idx, buf_size) do {           \
    for(int _i = (idx); _i < (buf_size) - 1; _i++) {    \
        buf[_i] = buf[_i + 1];                          \
    }                                                   \
    INVALID_ENTRY(buf, (buf_size) - 1);                 \
} while(0)                                              \

#define LINE(n) (line_buf + sizeof(line_buf) - (n))
#define SPACE(n) (space_buf + sizeof(space_buf) - (n))
const char line_buf[] = "-----------------------------------------------------------------------";
const char space_buf[] = "                                                                       ";

struct rob_entry {
    bool busy;
    bool issued;
    bool finished;
    int instr_addr;
    int tag;
    bool s;
};

struct reg_file_entry {
    int reg;
    int data;
    bool valid;
    bool busy;
};

struct pipeline_entry {
    int instr_addr;
    int tag;
    int val;
    int addr;
    int rob_i;
};

enum instr_type {
    NOP,
    ADD,
    SUB,
    AND,
    OR,
    NOT,
    LOAD,
    STORE,
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

struct instr {
    int type;
    int dst;
    int src1;
    int src2;
    int offset;
};

int front_rat[RAT_SIZE], back_rat[RAT_SIZE];
int mem[MEM_SIZE];

struct reg_file_entry reg_file[REG_FILE_SIZE];
struct rob_entry rob[ROB_SIZE];
struct instr_window_entry instr_window[INSTR_WINDOW_SIZE];
struct instr icache[MAX_INSTR_NUM + 1];

struct pipeline_entry alu_pipeline[ALU_PORT_NUM];
struct pipeline_entry load_pipeline[LOAD_PORT_NUM];
struct pipeline_entry store_pipeline[STORE_PORT_NUM];
struct pipeline_entry store_buffer[STORE_BUF_SIZE];
struct pipeline_entry completed_load_buffer[COMPLETED_LOAD_BUF_SIZE];

bool should_flush = FALSE;
int fault_ia = 0;
int instr_window_num = 0;
int finished_store = 0;
int committed_store = 0;
int completed_load = 0;
int rob_head = 0, rob_tail = 0, rob_size = 0;
int instr_num = 0;
int pc = 1;

char* type_to_str(int type) {
    switch (type) {
        case LOAD: return "lb";
        case STORE: return "sb";
        case ADD: return "add";
        case SUB: return "sub";
        case AND: return "and";
        case OR: return "or";
        default: return "";
    }
}

void invalidate_pipeline_entry(struct pipeline_entry *e) {
    memset(e, 0, sizeof(struct pipeline_entry));
}

void init() {
    // since we use global variables, there is no need to zeroize them now.
    for(int i = 0; i < MEM_SIZE; i++) {
        mem[i] = INIT_MEM_VAL(i);
    }

    for(int i = 0; i < REG_FILE_SIZE; i++){
        reg_file[i].data = INIT_REG_VAL(i);
        if (i < RAT_SIZE) {
            front_rat[i] = i;
            back_rat[i] = i;
            reg_file[i].reg = i;
            reg_file[i].busy = TRUE;
            reg_file[i].valid = TRUE;
        } else {
            reg_file[i].reg = -1;
        }  
    }
}

void flush() {
    printf("[Flush] start flushing with fault IA %d\n", fault_ia);
    for (int i = 0; i < ROB_SIZE; i++) {
        if (!rob[i].busy || !rob[i].s) {
            continue;
        }

        if (rob[i].tag != -1)
            INVALID_ENTRY(reg_file, rob[i].tag);

        if (rob[i].instr_addr == fault_ia)
            rob_tail = i;
            
        INVALID_ENTRY(rob, i);
            rob_size--;

        if (rob_size == 0)
            rob_head = rob_tail = 0;
    }

    for (int i = 0; i < completed_load; i++) {
        if (completed_load_buffer[i].instr_addr >= fault_ia) {
            printf("[Flush] flush completed load buffer IA %d\n", completed_load_buffer[i].instr_addr);
            REMOVE_ENTRY(completed_load_buffer, i, completed_load);
            completed_load--;
        }
    }

    for (int i = 0; i < finished_store; i++) {
        if (store_buffer[i].instr_addr >= fault_ia) {
            printf("[Flush] flush store buffer IA %d\n", store_buffer[i].instr_addr);
            REMOVE_ENTRY(store_buffer, i, finished_store);
            finished_store--;
        }
    }

    for (int i = 0; i < ALU_PORT_NUM; i++) {
        if (alu_pipeline[i].instr_addr >= fault_ia) {
            printf("[Flush] flush alu pipeline IA %d\n", alu_pipeline[i].instr_addr);
            invalidate_pipeline_entry(&alu_pipeline[i]);
        }
    }

    for (int i = 0; i < STORE_PORT_NUM; i++) {
        if (store_pipeline[i].instr_addr >= fault_ia) {
            printf("[Flush] flush store pipeline IA %d\n", store_pipeline[i].instr_addr);
            invalidate_pipeline_entry(&store_pipeline[i]);
        }
    }

    for (int i = 0; i < LOAD_PORT_NUM; i++) {
        if (load_pipeline[i].instr_addr >= fault_ia) {
            printf("[Flush] flush load pipeline IA %d\n", store_pipeline[i].instr_addr);
            invalidate_pipeline_entry(&load_pipeline[i]);
        }
    }

    for (int i = 0; i < instr_window_num; i++) {
        if (instr_window[i].instr_addr >= fault_ia) {
            printf("[Flush] flush instruction window IA %d\n", instr_window[i].instr_addr);
            REMOVE_ENTRY(instr_window, i, instr_window_num);
            instr_window_num--;
        }
    }

    for (int i = 0; i < RAT_SIZE; i++) {
        front_rat[i] = back_rat[i];
    }

    pc = fault_ia;
    fault_ia = 0;
}

void mark_speculative(int instr_addr) {
    bool start = FALSE;
    for(int n = 0, i = rob_head; n < rob_size; n++) {
        if (rob[i].busy && rob[i].instr_addr == instr_addr) {
            if (rob[i].s)
                return;
            start = TRUE;
        }

        if (start) {
            rob[i].s = TRUE;
            printf("[Retire] mark rob IA %d speculative\n", rob[i].instr_addr);
        }

        i = (i + 1) % ROB_SIZE;
    }

    fault_ia = instr_addr;
}

void commit_mem_instr(int instr_addr) {
    for(int i = 0; i < finished_store; i++) {
        if(store_buffer[i].instr_addr != instr_addr) {
            continue;
        }

        struct pipeline_entry e = store_buffer[i];
        REMOVE_ENTRY(store_buffer, i, finished_store + committed_store);
        store_buffer[finished_store + committed_store - 1] = e;
        finished_store--;
        committed_store++;

        for(int j = 0; j < completed_load; j++) {
            if (completed_load_buffer[i].addr == e.addr && completed_load_buffer[i].instr_addr > e.instr_addr) {
                should_flush = TRUE;
                mark_speculative(completed_load_buffer[i].instr_addr);
            }     
        }
        
        printf("[Retire] commit finished store IA %d\n", instr_addr);
        break;
    }

    for(int i = 0; i < completed_load; i++) {
        if (completed_load_buffer[i].instr_addr != instr_addr) {
            continue;
        }

        printf("[Retire] commit completed load IA %d\n", instr_addr);
        invalidate_pipeline_entry(&completed_load_buffer[i]);
        break;
    }
}

void flush_committed_store() {
    if (committed_store != 0)
        printf("[Retire] commit %d store to cache\n", committed_store);

    for(int i = 0; i < committed_store; i++) {
        int addr = store_buffer[finished_store + i].addr;
        int val = store_buffer[finished_store + i].val;

        mem[addr - MEM_BASE] = val;
        printf("[Retire] store %#x to address %#x\n", val, addr);
        invalidate_pipeline_entry(&store_buffer[finished_store + i]);
    }

    committed_store = 0;
}

void retire() {
    flush_committed_store();

    for(int i = 0; i < MAX_COMMIT && rob[rob_head].finished && rob[rob_head].busy && !rob[rob_head].s; i++) {
        int target_reg = reg_file[rob[rob_head].tag].reg;
        int evicted_tag = back_rat[target_reg];

        printf("[Retire] retire ROB IA %d\n", rob[rob_head].instr_addr);
       
        if(rob[rob_head].tag != -1) {
            printf("[Retire] Back End RAT $%d with new tag %d\n", target_reg, rob[rob_head].tag);
            printf("[Retire] invalidate Physical Register File $%d\n", evicted_tag);
            back_rat[target_reg] = rob[rob_head].tag;
            reg_file[evicted_tag].busy = FALSE;
            reg_file[evicted_tag].valid = FALSE;
        }
          
        commit_mem_instr(rob[rob_head].instr_addr);
        INVALID_ENTRY(rob, rob_head);

        rob_head = (rob_head + 1) % ROB_SIZE;
        rob_size--;
    }
}

void complete_alu() {
    for (int i = 0; i < ALU_PORT_NUM; i++) {
        if(!alu_pipeline[i].instr_addr)
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
    for(int i = 0; i < STORE_PORT_NUM && finished_store + committed_store < COMPLETED_LOAD_BUF_SIZE; i++) {
        if(!store_pipeline[i].instr_addr)
            continue;
        printf("[Complete] Update ROB IA %d finished\n", store_pipeline[i].instr_addr);
        rob[store_pipeline[i].rob_i].finished = TRUE;

        store_buffer[finished_store++] = store_pipeline[i];
        invalidate_pipeline_entry(&store_pipeline[i]);       
    }
    for(int i = 0; i < LOAD_PORT_NUM && completed_load < COMPLETED_LOAD_BUF_SIZE; i++) {
        if(!load_pipeline[i].instr_addr)
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
    int result = -1;

    switch (instr_window[i].instr) {
        case ADD: result = op1 + op2; break;
        case SUB: result = op1 - op2; break;
        case AND: result = op1 & op2; break;
        case OR: result = op1 | op2; break;
        default: break;
    }

    for(int j = 0; j < ALU_PORT_NUM; j++) {
        if(!alu_pipeline[j].instr_addr) {
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
        for(int j = 0; j < STORE_PORT_NUM; j++) {
            if(!store_pipeline[j].instr_addr) {
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
        for(int j = 0; j < LOAD_PORT_NUM; j++) {
            if(!load_pipeline[j].instr_addr) {
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
        case LOAD: case STORE: return op2_ready;
        default: return op1_ready && op2_ready;
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
            
            if(instr_window[i].instr == STORE && issued_store < STORE_PORT_NUM) {
                issued_store++;
                issue_mem(i);
            } else if (instr_window[i].instr == LOAD && issued_load < LOAD_PORT_NUM) {
                issued_load++;
                issue_mem(i);
            } else if (issued_alu < ALU_PORT_NUM){
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

    if (rob_size == ROB_SIZE) {
        DEBUG_PRINT("[Debug] rob full\n");
        return;
    }

    int i = instr_window_num;
    struct instr instr = icache[pc];
    int tag = -1;

    instr_window[i].op1 = instr.type == LOAD ? -1 : front_rat[instr.src1];
    instr_window[i].op2 = front_rat[instr.src2];

    if (instr.type != STORE) {
        tag = next_tag();
        if(tag == -1) {
            printf("[Rename] reg file full for IA %d\n", pc);
            return;
        }
        printf("[Rename] allocate Tag %d to IA %d with Reg %d\n", tag, pc, instr.dst);
        front_rat[instr.dst] = tag;
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
    rob_tail = (rob_tail + 1) % ROB_SIZE;
    rob_size++;
    pc++;
}

int parse_opcode(char* opcode){
    if (strcmp(opcode, "lb") ==  0|| strcmp(opcode, "load") == 0)
        return LOAD;

    if (strcmp(opcode, "sb") ==  0|| strcmp(opcode, "store") == 0)
        return STORE;

    if (strcmp(opcode, "add") == 0)
        return ADD;

    if (strcmp(opcode, "sub") == 0)
        return SUB;

    if (strcmp(opcode, "and") == 0)
        return AND;

    if (strcmp(opcode, "or") == 0)
        return OR;
    
    return -1;
}

void load_instrs() {
    printf("number of instructions: ");
    scanf("%d", &instr_num);

    for(int i = 1; i <= instr_num; i++){
        printf("enter instruction: ");
        char opcode[6] = {0};
        scanf("%s ", opcode);

        icache[i].type = parse_opcode(opcode);

        switch (icache[i].type) {
            case LOAD: {
                scanf("$%d, ", &icache[i].dst);
                scanf("%x($%d)", &icache[i].offset, &icache[i].src2);
            } break;
            case STORE: {
                scanf("$%d, ", &icache[i].src1);
                scanf("%x($%d)", &icache[i].offset, &icache[i].src2);
            } break;
            case ADD: case SUB: case AND: case OR: {
                scanf("$%d, $%d, $%d", &icache[i].dst, &icache[i].src1, &icache[i].src2);
            } break;
            default: printf("Invalid Opcode\n"); return;
        }
    }
    #ifdef DEBUG
    for(int i = 1; i <= instr_num; i++){
        switch (icache[i].type) {
            case LOAD: {
                printf("Load instr %s $%d, %#x($%d)\n", "ld", icache[i].dst, icache[i].offset, icache[i].src2);
            } break;
            case STORE: {               
                printf("Load instr %s $%d, %#x($%d)\n", "sb", icache[i].src1, icache[i].offset, icache[i].src2);
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

void print_reg() {
    putchar('\n');
    printf("%s[Front End RAT]%s[Back End RAT]%s[Physical Register File]\n", SPACE(4), SPACE(7), SPACE(8));
    printf("%s%s%s%s%s%s\n", SPACE(4), LINE(15), SPACE(8), LINE(15), SPACE(8), LINE(24));
    printf("%s|REG|     TAG|%s|REG|     TAG|%s|TAG|Reg| Data|  V|  B|\n", SPACE(4), SPACE(8), SPACE(8));
    printf("%s%s%s%s%s%s\n", SPACE(4), LINE(15), SPACE(8), LINE(15), SPACE(8), LINE(24));

    for(int i = 0; i < REG_FILE_SIZE; i++) {
        if (i < RAT_SIZE) {
            printf("%s|$%-2d|%8d|%s|$%-2d|%8d|", SPACE(4), i, front_rat[i], SPACE(8), i, back_rat[i]);
        } else {
            printf("%s", SPACE(39));
        }

        struct reg_file_entry e = reg_file[i];
        printf("%s|%3d|%3d|%#5x|%3d|%3d|\n", SPACE(8), i, e.reg, e.data, e.valid, e.busy);
        printf("%s%s%s%s", SPACE(4), i < RAT_SIZE ? LINE(15) : SPACE(15), SPACE(8), i < RAT_SIZE ? LINE(15) : SPACE(15));    
        printf("%s%s\n",SPACE(8), LINE(24));
    }

    putchar('\n');
}

#define PRINT_ROB_ROW(NAME, FIELD) do {                    \
    printf("%s%s", SPACE(8), NAME);                        \
    for(int i = 0; i < ROB_SIZE; i++) {                    \
        if (rob[i].busy)                                   \
            printf("%2d|", rob[i].FIELD);                  \
        else                                               \
            printf("  |");                                 \
    }                                                      \
    printf("\n%s%s\n", SPACE(8), LINE(3 * (ROB_SIZE + 2)));\
} while(0)                                                 \

void print_rob() {
    putchar('\n');
    printf("%s[ROB]\n", SPACE(8 + 3 * ROB_SIZE / 2));
    printf("%s%s\n", SPACE(8), LINE(3 * (ROB_SIZE + 2)));

    PRINT_ROB_ROW("|  B|", busy);
    PRINT_ROB_ROW("|  I|", issued);
    PRINT_ROB_ROW("|  F|", finished);
    PRINT_ROB_ROW("| IA|", instr_addr);
    PRINT_ROB_ROW("|TAG|", tag);
    PRINT_ROB_ROW("|  S|", s);

    putchar('\n');
}

void print_instr_window() {
    putchar('\n');
    printf("%s[Instruction Window]\n", SPACE(15));
    printf("%s%s\n", SPACE(8), LINE(35));
    printf("%s|IA|OutTag|Instr|OP1|OP2| Off|B|R|\n", SPACE(8));
    printf("%s%s\n", SPACE(8), LINE(35));

    for(int i = 0; i < instr_window_num; i++) {
        struct instr_window_entry e = instr_window[i];
        printf("%s", SPACE(8));
        printf("|%2d|%6d|%5s|%3d|%3d|%#4x|%1d|%1d|\n", e.instr_addr, e.out_tag, type_to_str(e.instr), e.op1, e.op2, e.off, e.busy,e.ready);
        printf("%s%s\n", SPACE(8), LINE(35));
    }

    putchar('\n');
}

void print_pipeline() {
    putchar('\n');
    printf("%s[ALU Pipeline]%s[Store Pipeline]%s[Load Pipeline]\n", SPACE(12), SPACE(17), SPACE(19));
    
    for(int i = 0; i < ALU_PORT_NUM; i++) {
        printf("%s%s%s%s%s%s\n", SPACE(8), LINE(24), SPACE(8), LINE(25), SPACE(8), LINE(29));
        printf("%s|stage|IA|TAG|(Result)|%s|stage|IA|(Addr)|(Data)|%s|stage|IA|TAG|(Addr)|(Data)|\n", SPACE(8), SPACE(8), SPACE(8));
        printf("%s%s%s%s%s%s\n", SPACE(8), LINE(24), SPACE(8), LINE(25), SPACE(8), LINE(29));

        struct pipeline_entry e = alu_pipeline[i];
        printf("%s", SPACE(8));
        printf("|%5d|%2d|%3d|%#8x|", 0, e.instr_addr, e.tag, e.val);
        printf("%s", SPACE(8));
        e = store_pipeline[i];
        printf("|%5d|%2d|%#6x|%#6x|", 0, e.instr_addr, e.addr, e.val);
        printf("%s", SPACE(8));
        e = load_pipeline[i];
        printf("|%5d|%2d|%3d|%#6x|%#6x|\n", 0, e.instr_addr, e.tag, e.addr, e.val);

        printf("%s%s%s%s%s%s\n", SPACE(8), LINE(24), SPACE(8), LINE(25), SPACE(8), LINE(29));
        putchar('\n');
    }
}

void print_mem_and_buffer() {
    putchar('\n');

    printf("%s[Store Buffer]%s[Completed Load Buffer]%s[Memory Contents]\n", SPACE(11), SPACE(8), SPACE(8));
    printf("%s%s%s%s%s%s\n", SPACE(11), LINE(15), SPACE(12), LINE(15), SPACE(13), LINE(16));

    printf("%s|IA|DATA|Addr|%s|IA|     Addr|%s|Address|Value|\n", SPACE(11), SPACE(12), SPACE(13));
    printf("%s%s%s%s%s%s\n", SPACE(11), LINE(15), SPACE(12), LINE(15), SPACE(13), LINE(16));

    int len = MEM_SIZE > STORE_BUF_SIZE ? MEM_SIZE : STORE_BUF_SIZE > COMPLETED_LOAD_BUF_SIZE ? STORE_BUF_SIZE : COMPLETED_LOAD_BUF_SIZE;
    
    for(int i = 0; i < len; i++) {
        printf("%s", i == finished_store ? "Committed " : i == 0 ? "Finished  " :  SPACE(11));
        if (i < STORE_BUF_SIZE) {
            printf("|%2d|%#4x|%4X|", store_buffer[i].instr_addr, store_buffer[i].val, store_buffer[i].addr);
            printf("%s", SPACE(12));
        } else {
            printf("%s", SPACE(26));
        }

        if (i < COMPLETED_LOAD_BUF_SIZE) {
            printf("|%2d|%9X|", completed_load_buffer[i - 2].instr_addr, completed_load_buffer[i - 2].addr);
            printf("%s", SPACE(13));
        } else {
            printf("%s", SPACE(27));
        }
        
        if (i < MEM_SIZE) {
            printf("|%#7x|%#5x|", MEM_BASE + i, mem[i]);
        }

        printf("\n%s%s", SPACE(11), i < STORE_BUF_SIZE ? LINE(15) : SPACE(15));
        printf("%s%s", SPACE(12), i < COMPLETED_LOAD_BUF_SIZE ? LINE(15) : SPACE(15));
        printf("%s%s\n", SPACE(13), i < MEM_SIZE ? LINE(16) : SPACE(16));
    }

    putchar('\n');
}

void print_info() {
    print_rob();
    print_instr_window();
    print_reg();
    print_pipeline();
    print_mem_and_buffer();
}

void validation() {
    flush_committed_store();
    int reg[RAT_SIZE];
    int mem_temp[MEM_SIZE];

    for (int i = 0; i < RAT_SIZE; i++)
        reg[i] = INIT_REG_VAL(i);

    for (int i = 0; i < MEM_SIZE; i++)
        mem_temp[i] = INIT_MEM_VAL(i);

    for (int i = 1; i <= instr_num; i++) {
        int dst = icache[i].dst, src1 = icache[i].src1, src2 = icache[i].src2, off = icache[i].offset;
        switch (icache[i].type) {
            case LOAD: reg[dst] = mem_temp[reg[src2] + off - MEM_BASE]; break;
            case STORE: mem_temp[reg[src2] + off - MEM_BASE] = reg[src1]; break;
            case ADD: reg[dst] = reg[src1] + reg[src2]; break;
            case SUB: reg[dst] = reg[src1] - reg[src2];break;
            case AND: reg[dst] = reg[src1] & reg[src2]; break;
            case OR: reg[dst] = reg[src1] | reg[src2]; break;
            default: printf("Validation Failed\n"); return;
        }  
    }

    bool is_success = TRUE;

    for (int i = 0; i < RAT_SIZE; i++) {
        is_success &= reg[i] == reg_file[back_rat[i]].data;
        if (reg[i] != reg_file[back_rat[i]].data)
            DEBUG_PRINT("[Debug] reg %d %#x | %#x\n", i, reg[i], reg_file[back_rat[i]].data);
    }

    for (int i = 0; i < MEM_SIZE; i++) {
        is_success &= mem_temp[i] == mem[i];
        if (mem_temp[i] != mem[i])
            DEBUG_PRINT("[Debug] mem %d %#x | %#x\n", i, mem_temp[i], mem[i]);
    }

    printf("%s\n", is_success ? "Validation Passed" : "Validation Failed");
}

void simulate_cycle() {
    if (should_flush) {
        flush();
        should_flush = FALSE;
        return;
    }

    retire();

    if (!DELAY_FLUSH && should_flush) {
        flush();
        should_flush = FALSE;
        return;
    }

    complete();

    issue();
    
    for (int i = 0; i < MAX_RENAME_DISPATCH; i++) {
        rename_dispatch();
        rename_dispatch();
    }
}

int main() {
    init();

    load_instrs();
    printf("press enter to continue..");
    getchar();
    getchar();

    int cycle = 0;

    while(pc <= instr_num || rob_size != 0) {
        #ifdef DEBUG
            system("clear");
            system("clear");
        #endif

        printf("%s\n\n#CYCLE %d, pc = %d\n\n", LINE(64), cycle, pc);

        simulate_cycle();

        print_info();

        printf("press enter to continue...");
        while(getchar() != '\n');
        putchar('\n');
        cycle++;
    }
    
    printf("%s\n", LINE(64));
    printf("End of Simulation\n");
    
    #ifdef VALIDATION
        validation();
    #endif
}