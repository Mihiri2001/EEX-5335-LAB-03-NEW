#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 8
#define NUM_PAGES 16
#define NUM_FRAMES 4
#define CACHE_SIZE 4
#define TLB_SIZE 2
#define VERBOSE 1

typedef struct {
    int frame;
    int valid;
    int dirty;
} PTE;

typedef struct {
    int vp;
    int pf;
} TLBEntry;

typedef struct {
    int pa;
    char data[50];
} CacheLine;

PTE pageTable[NUM_PAGES];
TLBEntry tlb[TLB_SIZE];
int tlbCount = 0;

CacheLine cache[CACHE_SIZE];
int cacheCount = 0;

char mainMemory[NUM_FRAMES * PAGE_SIZE][50];
char disk[NUM_PAGES][PAGE_SIZE][50];

int loadedPagesQueue[NUM_FRAMES];
int loadedCount = 0;

int tlbHits=0, tlbMisses=0, cacheHits=0, cacheMisses=0, pageFaults=0, diskWrites=0;

void initSystem() {
    for(int i=0;i<NUM_PAGES;i++){
        pageTable[i].frame = -1;
        pageTable[i].valid = 0;
        pageTable[i].dirty = 0;
        for(int w=0; w<PAGE_SIZE; w++){
            sprintf(disk[i][w], "DiskData(VP%d,W%d)", i, w);
        }
    }
    for(int i=0;i<NUM_FRAMES*PAGE_SIZE;i++){
        strcpy(mainMemory[i], "");
    }
}

int searchTLB(int vp){
    for(int i=0;i<tlbCount;i++){
        if(tlb[i].vp == vp){
            if(VERBOSE) printf("TLB HIT: VP %d -> PF %d\n", vp, tlb[i].pf);
            tlbHits++;
            return tlb[i].pf;
        }
    }
    if(VERBOSE) printf("TLB MISS for VP %d\n", vp);
    tlbMisses++;
    return -1;
}

void updateTLB(int vp, int pf){
    if(tlbCount < TLB_SIZE){
        tlb[tlbCount].vp = vp;
        tlb[tlbCount].pf = pf;
        tlbCount++;
    } else {
        // FIFO replacement
        for(int i=1;i<TLB_SIZE;i++) tlb[i-1]=tlb[i];
        tlb[TLB_SIZE-1].vp = vp;
        tlb[TLB_SIZE-1].pf = pf;
    }
}

int searchCache(int pa, char *data){
    for(int i=0;i<cacheCount;i++){
        if(cache[i].pa == pa){
            strcpy(data, cache[i].data);
            if(VERBOSE) printf("CACHE HIT at PA %d -> %s\n", pa, data);
            cacheHits++;
            return 1;
        }
    }
    cacheMisses++;
    if(VERBOSE) printf("CACHE MISS at PA %d\n", pa);
    return 0;
}

void insertCache(int pa, char *data){
    if(cacheCount < CACHE_SIZE){
        cache[cacheCount].pa = pa;
        strcpy(cache[cacheCount].data, data);
        cacheCount++;
    } else {
        // LRU: shift left
        for(int i=1;i<CACHE_SIZE;i++) cache[i-1]=cache[i];
        cache[CACHE_SIZE-1].pa = pa;
        strcpy(cache[CACHE_SIZE-1].data, data);
    }
}

int evictPageFIFO(){
    if(loadedCount==0) return -1;
    int vp = loadedPagesQueue[0];
    PTE *p = &pageTable[vp];
    int pf = p->frame;
    if(p->dirty){
        for(int w=0;w<PAGE_SIZE;w++){
            strcpy(disk[vp][w], mainMemory[pf*PAGE_SIZE+w]);
        }
        diskWrites++;
        if(VERBOSE) printf("Wrote back dirty VP %d to disk\n", vp);
    }
    p->valid=0; p->frame=-1; p->dirty=0;
    for(int i=1;i<loadedCount;i++) loadedPagesQueue[i-1]=loadedPagesQueue[i];
    loadedCount--;
    if(VERBOSE) printf("Evicted VP %d from PF %d\n", vp, pf);
    return pf;
}

int loadPageIntoFrame(int vp){
    pageFaults++;
    int pf = -1;
    int used[NUM_FRAMES]={0};
    for(int i=0;i<NUM_PAGES;i++) if(pageTable[i].valid) used[pageTable[i].frame]=1;
    for(int f=0;f<NUM_FRAMES;f++){ if(!used[f]) { pf=f; break; } }
    if(pf==-1) pf=evictPageFIFO();
    pageTable[vp].frame = pf;
    pageTable[vp].valid = 1;
    pageTable[vp].dirty = 0;
    for(int w=0; w<PAGE_SIZE; w++){
        strcpy(mainMemory[pf*PAGE_SIZE + w], disk[vp][w]);
    }
    loadedPagesQueue[loadedCount++] = vp;
    if(VERBOSE) printf("Loaded VP %d -> PF %d\n", vp, pf);
    return pf;
}

int getPhysicalAddress(int va, int isWrite){
    int vp = va / PAGE_SIZE;
    int offset = va % PAGE_SIZE;
    int pf = searchTLB(vp);
    if(pf==-1){
        if(!pageTable[vp].valid) pf = loadPageIntoFrame(vp);
        else pf = pageTable[vp].frame;
        updateTLB(vp, pf);
    }
    if(isWrite) pageTable[vp].dirty=1;
    return pf*PAGE_SIZE + offset;
}

char* readAddress(int va){
    if(VERBOSE) printf("\n=== READ VA %d ===\n", va);
    int pa = getPhysicalAddress(va, 0);
    static char data[50];
    if(searchCache(pa,data)) return data;
    strcpy(data, mainMemory[pa]);
    insertCache(pa,data);
    if(VERBOSE) printf("Inserted into cache PA %d -> %s\n", pa, data);
    return data;
}

void writeAddress(int va, char *newData){
    if(VERBOSE) printf("\n=== WRITE VA %d := %s ===\n", va, newData);
    int pa = getPhysicalAddress(va, 1);
    strcpy(mainMemory[pa], newData);
    insertCache(pa,newData);
    if(VERBOSE) printf("Updated cache line for PA %d\n", pa);
}

int main(){
    initSystem();
    // sample trace
    int traceVA[] = {0,1,2,15,16,31,32,33,64,65,95,96,120,0,16,96,2};
    char *traceOp[] = {"R","R","R","R","W","R","R","R","R","R","R","W","R","R","R","R","R"};
    char *traceData[] = {NULL,NULL,NULL,NULL,"Modified(VP2,W0)",NULL,NULL,NULL,NULL,NULL,NULL,"Modified(VP12,W0)",NULL,NULL,NULL,NULL,NULL};

    int n = sizeof(traceVA)/sizeof(traceVA[0]);
    for(int i=0;i<n;i++){
        if(traceOp[i][0]=='R'){
            char *data = readAddress(traceVA[i]);
            printf("-> Read result: %s\n", data);
        } else {
            writeAddress(traceVA[i], traceData[i]);
            printf("-> Wrote data to VA %d\n", traceVA[i]);
        }
    }

    printf("\n=== FINAL STATS ===\n");
    printf("TLB Hits: %d | TLB Misses: %d\n", tlbHits, tlbMisses);
    printf("Cache Hits: %d | Cache Misses: %d\n", cacheHits, cacheMisses);
    printf("Page Faults: %d | Disk Writes (write-backs): %d\n", pageFaults, diskWrites);
    printf("====================\n");
    return 0;
}
