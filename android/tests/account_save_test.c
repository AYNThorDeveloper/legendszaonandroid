/* LISDE account and save overloads (account.c, fs_host.c): OpenUser only for the local user, qualified users, the user
   selector, Push/PopOpenUsers, uid-only save existence / ensure / extend, CommitSaveData and cache storage sizes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
void *runtime_account_override(const char *name);
void *runtime_fs_host_override(const char *name);
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
int runtime_romfs_owns(const char *name) { (void)name; return 0; }
void runtime_fs_log(const char *a, const char *b) { (void)a; (void)b; }
uint64_t runtime_now_ns(void) { return 0; }
/* RomFS and save-probe hooks are not reached here */
#define UNREACHED(n) void n(void) { fprintf(stderr, #n " reached\n"); abort(); }
UNREACHED(dm_guest_write_prepare) UNREACHED(runtime_autoload_save_file_opened) UNREACHED(runtime_romfs_close_directory) UNREACHED(runtime_romfs_close)
UNREACHED(runtime_romfs_directory_count) UNREACHED(runtime_romfs_directory_handle) UNREACHED(runtime_romfs_entry_type) UNREACHED(runtime_romfs_handle)
UNREACHED(runtime_romfs_open_directory) UNREACHED(runtime_romfs_open) UNREACHED(runtime_romfs_read_directory) UNREACHED(runtime_romfs_read) UNREACHED(runtime_romfs_size)

typedef struct { uint64_t lo, hi; } Uid;
typedef struct { Uid uid; uint64_t token; } UserHandle;
#define F(T, name) ((T)need_fn(name))
static void *need_fn(const char *name) { void *f = runtime_account_override(name); if (!f) f = runtime_fs_host_override(name); if (!f) { fprintf(stderr, "no override %s\n", name); abort(); } return f; }

int main(void) {
    char dir[] = "/tmp/lisde-account-save-XXXXXX"; assert(mkdtemp(dir));
    setenv("TOTK_PROFILE_DIR", dir, 1);
    F(void (*)(void), "_ZN2nn7account10InitializeEv")();
    Uid me; int count = 0;
    assert(!F(uint32_t (*)(int *, Uid *, int), "_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi")(&count, &me, 8) && count == 1 && (me.lo || me.hi));
    Uid picked = {0}; assert(!F(uint32_t (*)(Uid *, const void *), "_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE")(&picked, NULL));
    assert(picked.lo == me.lo && picked.hi == me.hi);
    UserHandle h; Uid other = {me.lo ^ 1, me.hi};
    uint32_t (*open_user)(UserHandle *, const Uid *) = F(uint32_t (*)(UserHandle *, const Uid *), "_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE");
    assert(open_user(&h, &other) != 0);
    assert(!open_user(&h, &me) && h.uid.lo == me.lo);
    Uid back; assert(!F(uint32_t (*)(Uid *, const UserHandle *), "_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE")(&back, &h) && back.lo == me.lo);
    uint32_t (*pop)(int *, UserHandle *, int) = F(uint32_t (*)(int *, UserHandle *, int), "_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi");
    UserHandle popped; assert(!pop(&count, &popped, 1) && count == 0);   /* fresh launch: nothing pushed */
    assert(!F(uint32_t (*)(void), "_ZN2nn7account13PushOpenUsersEv")());
    assert(!pop(&count, &popped, 1) && count == 1 && popped.uid.lo == me.lo);
    assert(!pop(&count, &popped, 1) && count == 0);   /* popped once */
    F(void (*)(const UserHandle *), "_ZN2nn7account9CloseUserERKNS0_10UserHandleE")(&h);
    F(void (*)(const UserHandle *), "_ZN2nn7account9CloseUserERKNS0_10UserHandleE")(&popped);

    unsigned char (*exists)(const Uid *) = F(unsigned char (*)(const Uid *), "_ZN2nn2fs18IsSaveDataExistingERKNS_7account3UidE");
    assert(exists(&me) == 1 && exists(&other) == 0);
    assert(!F(uint32_t (*)(const Uid *), "_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE")(&me));
    assert(F(uint32_t (*)(const Uid *), "_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE")(&other) != 0);
    assert(!F(uint32_t (*)(const Uid *, int64_t, int64_t), "_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll")(&me, 1 << 20, 1 << 16));
    assert(!F(uint32_t (*)(const char *, const Uid *), "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE")("save", &me));
    assert(!F(uint32_t (*)(const char *), "_ZN2nn2fs14CommitSaveDataEPKc")("save"));
    assert(F(uint32_t (*)(const char *), "_ZN2nn2fs14CommitSaveDataEPKc")("nosuch") != 0);
    int64_t size = 0, journal = 0;
    uint32_t (*cache_size)(int64_t *, int64_t *, int) = F(uint32_t (*)(int64_t *, int64_t *, int), "_ZN2nn2fs19GetCacheStorageSizeEPlS1_i");
    assert(cache_size(&size, &journal, 3) != 0);
    assert(!F(uint32_t (*)(int, int64_t, int64_t), "_ZN2nn2fs18CreateCacheStorageEill")(3, 64 << 20, 4 << 20));
    assert(!cache_size(&size, &journal, 3) && size == 64 << 20 && journal == 4 << 20);
    uint64_t list; int n; struct { int32_t index; uint8_t reserved[0x1c]; } info[4];
    assert(!F(uint32_t (*)(uint64_t *), "_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE")(&list));
    assert(!F(uint32_t (*)(int *, void *, uint64_t, int), "_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi")(&n, info, list, 4));
    assert(n == 1 && info[0].index == 3);   /* the size record is not listed as a cache storage */
    F(void (*)(uint64_t), "_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE")(list);
    char cmd[128]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); assert(!system(cmd));
    puts("PASS: OpenUser/ListQualifiedUsers/ShowUserSelector return only the local user; Pop after Push returns it once; uid save existence/ensure/extend/commit; cache storage size recorded and not listed.");
}
