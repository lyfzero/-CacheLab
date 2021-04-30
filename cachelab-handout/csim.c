#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <limits.h>
#include <string.h>
#include <errno.h>

#include "cachelab.h"

//#define DEBUG_ON
 
// 内存地址
typedef unsigned long long int mem_addr_t;
 
// Cache 行
typedef struct cache_line {
    char valid;
    mem_addr_t tag;
} cache_line_t;
 
typedef cache_line_t* cache_set_t; // 指向一个set的地址
typedef cache_set_t* cache_t;      // 指向cache的地址

// cache 模拟器
cache_t cache;
mem_addr_t set_index_mask;

// LRU node
typedef struct LRU_line {
    int index;                    // set中的block index
    struct LRU_line* prev;
    struct LRU_line* next;
} LRU_line_t;
typedef LRU_line_t* LRU_set_t;  // 指向一个set的LRU链表的地址
typedef LRU_set_t* LRU_t;

// LRU 模拟
LRU_t LRU;

// 通过命令行设置的参数
int verbosity = 0; // 是否输出 trace
int s = 0;         // set index bits
int b = 0;         // block offset bits
int E = 0;         // associativity
char* trace_file = NULL;
 
// 由命令行参数计算的参数
int S;             // number of sets 
int B;             // block size (bytes) 

// 保存 cache 记录的计数器
int miss_count = 0;
int hit_count = 0;
int eviction_count = 0;
 
/*
 * init 初始化cache，分配内存，初始值设为0；初始化lru链表
 * 同时计算 set_index_mask
 */
void initCache() {
    // init cache
    cache = (cache_set_t*) malloc(sizeof(cache_set_t) * S);
    for (int i = 0; i < S; i++){
        cache[i] = (cache_line_t*) malloc(sizeof(cache_line_t) * E);
        for (int j = 0; j < E; j++){
            cache[i][j].valid = 0;
            cache[i][j].tag = 0;
        }
    }
    // 计算 set index mask
    set_index_mask = (mem_addr_t) (pow(2, s) - 1);

    // init LRU
    LRU = (LRU_set_t*) malloc(sizeof(LRU_set_t) * S);
    for (int i = 0; i < S; i++) {
        LRU[i] = (LRU_line_t*)malloc(sizeof(LRU_line_t)); // head
    }
}
 
 
/*
 * freeCache - 释放分配的内存
 */
void freeCache() {
    for(int i = 0; i < S; i++) {
        free(cache[i]);
        LRU_line_t* p = LRU[i];
        while(p != NULL) {
            LRU_line_t* tmp = p;
            p = p->next;
            free(tmp);
        }
    }
    free(cache);
    free(LRU);
}


/*
 * 将第index个block对应的lru节点放入链表头部
 */
void getToHead(int set_index, int index) {
    LRU_line_t* lruP = LRU[set_index];
    if(lruP->next != NULL && lruP->next->index == index) { // 已经在第一个
        return;
    }
    while(lruP->next != NULL) {
        lruP = lruP->next;
        if(lruP->index == index) {    // 找到对应的lru节点
            lruP->prev->next = lruP->next;
            if(lruP->next != NULL) {
                lruP->next->prev = lruP->prev;
            }
            lruP->next = LRU[set_index]->next;
            lruP->prev = LRU[set_index];
            LRU[set_index]->next->prev = lruP;
            LRU[set_index]->next = lruP;
            break;
        }
    }
}
/*
 * 将LRU表尾的节点放到链表头部（即清除数据再放入新数据），返回这个节点对应的index
 */
int popTailToHead(int set_index) {
    LRU_line_t* lruP = LRU[set_index];
    while(lruP->next != NULL) { // 找到表尾节点
        lruP = lruP->next;
    }
    // 放入头部
    lruP->prev->next = NULL;
    lruP->next = LRU[set_index]->next;
    lruP->prev = LRU[set_index];
    if(LRU[set_index]->next != NULL) {
        LRU[set_index]->next->prev = lruP;
    }
    LRU[set_index]->next = lruP;
    return lruP->index;
}
/*
 * 插入新的节点到链表头部
 */
void insertToHead(int set_index, int index) {
    LRU_line_t* newNode = (LRU_line_t*)malloc(sizeof(LRU_line_t));
    newNode->index = index;
    if(LRU[set_index]->next != NULL) {
        LRU[set_index]->next->prev = newNode;
    }
    newNode->next = LRU[set_index]->next;
    LRU[set_index]->next = newNode;
    newNode->prev = LRU[set_index];
}

/*
 * accessData - 访问给定主存地址的数据
 * 如果已经在 cache 中，命中，增加 hit_count
 * 如果不在 cache 中，未命中，放入 cache，增加 miss_count
 * 如果这行 evicted，增加eviction_count
 */
void accessData(mem_addr_t addr) {
    mem_addr_t set_index = (addr >> b) & set_index_mask;
    mem_addr_t tag = addr >> (s + b);
 
    cache_set_t cache_set = cache[set_index];  // 取出对应的 set
    // 判断是否命中
    for (int i = 0; i < E; ++i) {
        // 命中
        if (cache_set[i].valid && cache_set[i].tag == tag) {
            if (verbosity)
                printf("hit ");
            ++hit_count;
            getToHead(set_index, i);  // 将其lru节点放入表头
            return;
        }
    }

    // 未命中
    if (verbosity)
        printf("miss ");
    ++miss_count;

    // 找出最大lru的block 或者第一个非法行
    int j;
    for (j = 0; j < E; ++j) {    
        if(cache_set[j].valid != 1) {  // 找到第一个invalid的行
            cache_set[j].valid = 1;
            cache_set[j].tag = tag;
            insertToHead(set_index, j);   // 新建lru节点放入表头
            break;
        }
    }
    if (j == E) { // 所有的行都是valid的，需要替换 lru 最大的，即 lru 表尾的
        if (verbosity)
            printf("eviction ");
        ++eviction_count;
        int index = popTailToHead(set_index);
        cache_set[index].tag = tag;
        cache_set[index].valid = 1;
    }
}


/*
 * parseTrace - 在cache上模拟给定的访存轨迹
 */
void parseTrace(char* trace_fn) {
    char ch;
    mem_addr_t addr = 0;
    unsigned int size = 0;
    FILE* trace_fp = fopen(trace_fn, "r");  // 打开 trace 文件
    if(trace_fp == NULL) {
        printf("file open error");
        exit(1);
    }
    while (fscanf(trace_fp, " %c %llx,%d", &ch, &addr, &size) > 0) {
        if (verbosity && ch != 'I')
            printf("%c %llx,%d ", ch, addr, size);
        switch (ch) {
            case 'I':
                break;
            case 'L':
            case 'S':
                accessData(addr);
                break;
            case 'M': // 两次访存
                accessData(addr);
                accessData(addr);
                break;
            default:
                break;
        }
        if (verbosity && ch != 'I')
            putchar('\n');
    }
    fclose(trace_fp);
}
 
/*
 * printUsage - 打印 usage 信息
 */
void printUsage(char* argv[]) {
    printf("Usage: %s [-hv] -s <num> -E <num> -b <num> -t <file>\n", argv[0]);
    printf("Options:\n");
    printf("  -h         Print this help message.\n");
    printf("  -v         Optional verbose flag.\n");
    printf("  -s <num>   Number of set index bits.\n");
    printf("  -E <num>   Number of lines per set.\n");
    printf("  -b <num>   Number of block offset bits.\n");
    printf("  -t <file>  Trace file.\n");
    printf("\nExamples:\n");
    printf("  linux>  %s -s 4 -E 1 -b 4 -t traces/yi.trace\n", argv[0]);
    printf("  linux>  %s -v -s 8 -E 2 -b 4 -t traces/yi.trace\n", argv[0]);
    exit(0);
}
 
/*
 * main
 */
int main(int argc, char* argv[]) {
    char opt; 
    while( (opt = getopt(argc, argv, "s:E:b:t:vh")) != -1){
        switch(opt){
            case 's':
                s = atoi(optarg);
                break;
            case 'E':
                E = atoi(optarg);
                break;
            case 'b':
                b = atoi(optarg);
                break;
            case 't':
                trace_file = optarg;
                break;
            case 'v':
                verbosity = 1;
                break;
            case 'h':
                printUsage(argv);
                exit(0);
            default:
                printUsage(argv);
                exit(1);
        }
    }
 
    // 保证所有必须的命令行参数已指定
    if (s == 0 || E == 0 || b == 0 || trace_file == NULL) {
        printf("%s: Missing required command line argument\n", argv[0]);
        printUsage(argv);
        exit(1);
    }
 
    // 根据命令行输入的参数计算 S 和 B 
    S = (unsigned int) pow(2, s);
    B = (unsigned int) pow(2, b);
  
    // 初始化 cache
    initCache();
 
#ifdef DEBUG_ON
    printf("DEBUG: S:%u E:%u B:%u trace:%s\n", S, E, B, trace_file);
    printf("DEBUG: set_index_mask: %llu\n", set_index_mask);
#endif

    parseTrace(trace_file);
 
    // // 释放分配的空间
    freeCache();
 
    // 输出命中和未命中相关信息
    printSummary(hit_count, miss_count, eviction_count);
    return 0;
}