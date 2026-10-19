/* Offline AppContent provider for the configured base-game profile.
 * Add-ons (DLC): BB_ADDCONT lists extracted add-on folders (':'-separated, each with
 * sce_sys/param.sfo of CATEGORY "ac"); each is reported installed under the entitlement label of
 * its CONTENT_ID. No downloads or license checks. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile, no mounted add-ons");
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
/* SceAppContentAddcontInfo: the entitlement label (16 characters, NUL, 3 padding) and the
 * download status (4: installed). */
typedef struct { char label[17]; char padding[3]; uint32_t status; } AddonInfo;
enum { MAX_ADDONS=32, ADDON_INSTALLED=4 };
static AddonInfo addons[MAX_ADDONS];
static unsigned addon_count, addons_read;
/* CONTENT_ID of a param.sfo, or 0. */
static int sfo_content_id(const char *path, char out[64]) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char d[8192]; size_t n=fread(d,1,sizeof(d),f); fclose(f);
    if (n<20 || memcmp(d,"\0PSF",4)) return 0;
    uint32_t keys,data,count; memcpy(&keys,d+8,4); memcpy(&data,d+12,4); memcpy(&count,d+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=d+20+i*16; uint16_t ko; uint32_t len,off;
        memcpy(&ko,e,2); memcpy(&len,e+4,4); memcpy(&off,e+12,4);
        if (keys+ko>=n || data+off+len>n) continue;
        if (strcmp((const char *)d+keys+ko,"CONTENT_ID")) continue;
        if (len>=64) len=63;
        memcpy(out,d+data+off,len); out[len]=0; return 1;
    }
    return 0;
}
void runtime_content_read_addons(const char *spec) {
    addons_read=1;
    if (!spec || !*spec) return;
    char copy[4096]; snprintf(copy,sizeof(copy),"%s",spec);
    for (char *dir=strtok(copy,":"); dir && addon_count<MAX_ADDONS; dir=strtok(NULL,":")) {
        char path[4200], id[64];
        snprintf(path,sizeof(path),"%s/sce_sys/param.sfo",dir);
        /* XXYYYY-CUSAnnnnn_00-LABEL: the label is the 16 characters after the last '-'. */
        const char *label=sfo_content_id(path,id) ? strrchr(id,'-') : NULL;
        if (!label || strlen(label+1)!=16) { printf("Runtime: add-on %s: no CONTENT_ID with a 16-character label, skipped\n",dir); continue; }
        AddonInfo *a=&addons[addon_count++];
        memset(a,0,sizeof(*a)); memcpy(a->label,label+1,16); a->status=ADDON_INSTALLED;
        printf("Runtime: add-on %s installed (%s)\n",a->label,id);
    }
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    if (!addons_read) runtime_content_read_addons(getenv("BB_ADDCONT"));
    /* Never touch list slots past what is reported. */
    unsigned shown=addon_count;
    if (list && capacity) {
        if (shown>capacity) shown=capacity;
        memcpy(list,addons,shown*sizeof(AddonInfo));
    }
    if (hits) *hits=list && capacity ? shown : addon_count;
    ++lists; return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#M#N")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#M#N")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#M#N")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#c#d")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#c#d")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#c#d")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
