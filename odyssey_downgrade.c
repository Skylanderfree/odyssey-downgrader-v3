/*
 * main.c
 *
 * Copyright (c) 2020, DarkMatterCore <pabloacurielz@gmail.com>.
 *
 * This file is part of nxdumptool (https://github.com/DarkMatterCore/nxdumptool).
 *
 * nxdumptool is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nxdumptool is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <core/nxdt_utils.h>
#include <core/bktr.h>
#include <core/gamecard.h>
#include <core/usb.h>
#include <core/title.h>
#include <core/pfs.h>
#include <core/romfs.h>

#define RESET   "\033[0m"
#define BLACK   "\033[30m"      /* Black */
#define RED     "\033[31m"      /* Red */
#define GREEN   "\033[32m"      /* Green */
#define YELLOW  "\033[33m"      /* Yellow */
#define BLUE    "\033[34m"      /* Blue */
#define MAGENTA "\033[35m"      /* Magenta */
#define CYAN    "\033[36m"      /* Cyan */
#define WHITE   "\033[37m"      /* White */
#define BOLDBLACK   "\033[1m\033[30m"      /* Bold Black */
#define BOLDRED     "\033[1m\033[31m"      /* Bold Red */
#define BOLDGREEN   "\033[1m\033[32m"      /* Bold Green */
#define BOLDYELLOW  "\033[1m\033[33m"      /* Bold Yellow */
#define BOLDBLUE    "\033[1m\033[34m"      /* Bold Blue */
#define BOLDMAGENTA "\033[1m\033[35m"      /* Bold Magenta */
#define BOLDCYAN    "\033[1m\033[36m"      /* Bold Cyan */
#define BOLDWHITE   "\033[1m\033[37m"      /* Bold White */

#define BLOCK_SIZE  USB_TRANSFER_BUFFER_SIZE
#define R_PATH_EXISTS (0x402)
#define R_PATH_DOESNT_EXIST (0x202)

bool g_borealisInitialized = false;

static PadState g_padState = {0};

static Mutex g_fileMutex = 0;
static CondVar g_readCondvar = 0, g_writeCondvar = 0;

typedef struct
{
    FILE *fd;
    NcaFsSectionContext* section_ctx;

    void *data;
    size_t data_size;
    size_t data_written;

    size_t total_offset;
    size_t total_size;

    bool read_error;
    bool write_error;
    bool transfer_cancelled;
    bool mode;
} ThreadSharedData;

static void utilsScanPads(void)
{
    padUpdate(&g_padState);
}

static u64 utilsGetButtonsDown(void)
{
    return padGetButtonsDown(&g_padState);
}

static u64 utilsGetButtonsHeld(void)
{
    return padGetButtons(&g_padState);
}

static void utilsWaitForButtonPress(u64 flag)
{
    /* Don't consider stick movement as button inputs. */
    if (!flag) flag = ~(HidNpadButton_StickLLeft | HidNpadButton_StickLRight | HidNpadButton_StickLUp | HidNpadButton_StickLDown | HidNpadButton_StickRLeft | HidNpadButton_StickRRight | \
                        HidNpadButton_StickRUp | HidNpadButton_StickRDown);

    while(appletMainLoop())
    {
        utilsScanPads();
        if (utilsGetButtonsDown() & flag) break;
    }
}

static void consolePrint(const char *text, ...)
{
    va_list v;
    va_start(v, text);
    vfprintf(stdout, text, v);
    va_end(v);
    fflush(stdout);
    consoleUpdate(NULL);
}

// making my own consoleClear since libnx removed it
static void utilsConsoleClear(void)
{
    printf("\x1b[2J\x1b[H");
    fflush(stdout);
    consoleUpdate(NULL);
}

// Redirect all consoleClear() to this implementation.
#ifdef consoleClear
#undef consoleClear
#endif
#define consoleClear() utilsConsoleClear()

// Prevent flickering when in menu or selecting an option
static void utilsConsoleClearNoUpdate(void)
{
    printf("\x1b[2J\x1b[H");
    fflush(stdout);
}

static const char sdmc_prefix[] = "sdmc:";
static const size_t sdmc_prefix_length = sizeof(sdmc_prefix) - 1;
static const char romfs_path[] = "sdmc:/atmosphere/contents/0100000000010000/romfs.bin";
static const char exefs_path[] = "sdmc:/atmosphere/contents/0100000000010000/exefs.nsp";
static const char patch_path[] = "sdmc:/atmosphere/exefs_patches/odyssey_100_downgrade/3CA12DFAAF9C82DA064D1698DF79CDA1.ips";

static const u8 patch_data[] = {
  0x49, 0x50, 0x53, 0x33, 0x32, 0x00, 0x53, 0x70, 0x44, 0x00, 0x04, 0x00,
  0x60, 0xB8, 0x52, 0x00, 0x53, 0x70, 0x68, 0x00, 0x04, 0x06, 0x00, 0x00,
  0x14, 0x45, 0x45, 0x4F, 0x46
};

static const size_t patch_data_size = sizeof(patch_data);

/*
 * Removes a file from the SD card no matter how it was created:
 *  FAT32: big files are ConcatenationFiles (a directory with the archive bit set).
 *  exFAT / small files: regular files.
 * Returns 0 if the file is gone (or never existed), or the libnx result code otherwise.
 */
static int remove_sd_file_any(const char *path)
{
    FsFileSystem *fs = utilsGetSdCardFileSystemObject();

    // Handles ConcatenationFiles if the path doesn't exist or is a regular file
    utilsRemoveConcatenationFile(path);

    // Handles regular files
    int r = fsFsDeleteFile(fs, path + sdmc_prefix_length);
    if (R_FAILED(r) && r != R_PATH_DOESNT_EXIST) {
        // checks for any leftover files just in case
        int r2 = fsFsDeleteDirectoryRecursively(fs, path + sdmc_prefix_length);
        if (R_SUCCEEDED(r2)) r = 0; } \
    else
    if (r == R_PATH_DOESNT_EXIST) {
        r = 0; }
    utilsCommitSdCardFileSystemChanges();
    return r;
}

static void read_thread_func(void *arg)
{
    ThreadSharedData *shared_data = (ThreadSharedData*)arg;
    u8 *buf = NULL;
    const char *path = NULL;

    if (!shared_data || !shared_data->data || !shared_data->total_size || (!shared_data->section_ctx))
    {
        if (shared_data) shared_data->read_error = true;
        goto end;
    }

    buf = malloc(BLOCK_SIZE);
    if (!buf)
    {
        shared_data->read_error = true;
        goto end;
    }

    path = shared_data->mode ? exefs_path : romfs_path;

    // Use the same approach as nxdumptool to only use the ConcatenationFile if the file is bigger than the FAT32 limit
    // exFAT the file system driver doesn't need it, and on FAT32 it is required
    if (shared_data->total_size > FAT32_FILESIZE_LIMIT)
    {
        // Make sure there are no leftover files from a previous attempt
        remove_sd_file_any(path);

        if (!utilsCreateConcatenationFile(path))
        {
            consolePrint("create concatenation file failed\n");
            shared_data->read_error = true;
            condvarWakeAll(&g_writeCondvar);
            goto cleanup;
        }
    } else {
        // Small file: make sure don't write over a leftover directory/file
        remove_sd_file_any(path);
    }

    /* Open file */
    shared_data->read_error = ((shared_data->fd = fopen(path, "wb")) == NULL);
    if (shared_data->read_error)
    {
        consolePrint("failed to open output file\n");
        condvarWakeAll(&g_writeCondvar);
        goto cleanup;
    }

    // Unbuffered writes + preallocation 
    setvbuf(shared_data->fd, NULL, _IONBF, 0);
    ftruncate(fileno(shared_data->fd), (off_t)shared_data->total_size);

    for(u64 offset = 0, blksize = BLOCK_SIZE; offset < shared_data->total_size; offset += blksize)
    {
        if ((shared_data->total_size - offset) < blksize) blksize = (shared_data->total_size - offset);

        // Check if the transfer has been cancelled by the user
        if (shared_data->transfer_cancelled)
        {
            condvarWakeAll(&g_writeCondvar);
            break;
        }

        // Read current file
        shared_data->read_error = !ncaReadFsSection(shared_data->section_ctx, buf, blksize, offset + shared_data->total_offset);
        if (shared_data->read_error)
        {
            condvarWakeAll(&g_writeCondvar);
            break;
        }
        mutexLock(&g_fileMutex);

        if (shared_data->data_size && !shared_data->write_error) condvarWait(&g_readCondvar, &g_fileMutex);
        if (shared_data->write_error)
        {
            mutexUnlock(&g_fileMutex);
            break;
        }

        // Copy file data chunk to the buffer
        memcpy(shared_data->data, buf, blksize);
        shared_data->data_size = blksize;
        mutexUnlock(&g_fileMutex);
        condvarWakeAll(&g_writeCondvar);
    }

    // Wait until the previous file data chunk has been written
    mutexLock(&g_fileMutex);
    if (shared_data->data_size && !shared_data->write_error && !shared_data->transfer_cancelled) condvarWait(&g_readCondvar, &g_fileMutex);
    mutexUnlock(&g_fileMutex);
cleanup:
    free(buf);

end:
    threadExit();
}

static void write_thread_func(void *arg)
{
    ThreadSharedData *shared_data = (ThreadSharedData*)arg;
    if (!shared_data || !shared_data->data)
    {
        if (shared_data) shared_data->write_error = true;
        goto end;
    }

    while(shared_data->data_written < shared_data->total_size)
    {
        /* Wait until the current file data chunk has been read */
        mutexLock(&g_fileMutex);

        if (!shared_data->data_size && !shared_data->read_error) condvarWait(&g_writeCondvar, &g_fileMutex);

        if (shared_data->read_error || shared_data->transfer_cancelled || !shared_data->fd)
        {
            mutexUnlock(&g_fileMutex);
            shared_data->data_size = 0;
            condvarWakeAll(&g_readCondvar);
            break;
        }

        /* Write current file data chunk */
        shared_data->write_error = (fwrite(shared_data->data, 1, shared_data->data_size, shared_data->fd) != shared_data->data_size);
        if (!shared_data->write_error)
        {
            shared_data->data_written += shared_data->data_size;
            shared_data->data_size = 0;
        }

        /* Wake up the read thread to continue reading data */
        mutexUnlock(&g_fileMutex);
        condvarWakeAll(&g_readCondvar);

        if (shared_data->write_error) break;
    }

end:
    threadExit();
}

u8 get_program_id_offset(TitleInfo *info, u32 program_count)
{
    return 0;
}

static bool check_sd_space(u64 required_size) {

    FsFileSystem *fs = utilsGetSdCardFileSystemObject();
    s64 free_space = 0;
    int r = fsFsGetFreeSpace(fs, "/", &free_space);

    if (R_FAILED(r))
    {
        consolePrint("failed to check SD card space (%x)\n", r);
        return false;
    }

    double required_gb = (double)required_size / (1024.0 * 1024.0 * 1024.0);
    double free_gb = (double)free_space / (1024.0 * 1024.0 * 1024.0);

    if (free_space < (s64)required_size)
    {
        consolePrint(BOLDYELLOW "not enough space on SD card\n" RESET);
        consolePrint("required: %.2f GB\n", required_gb);
        consolePrint("available: %.2f GB\n", free_gb);
        return false;
    }
    return true;
}

//checks the exefs.nsp checksum
typedef struct
{
    const char *label;  // names EX: "1.0.0 digital"
    const char *hash;   // letters for SHA-256
} KnownHash;

//checks known hashes at the moment
static const KnownHash known_exefs_hashes[] = {
    { "1.0.0 Physical",  "2a355aace93ebb99df97bdcc2da664d8c27434b5396adc1266b4753fadc378d7" },
    { "1.0.0 Physical",  "8d197e879e29936424b49def2b29cdaf18017d9796f48238f8794626df52b473" },
    { "1.3.0 Physical",  "3b266d58d6829c9cea1699fe79906b77ec98fc1f842e77d7064d5187a9cd432c" },
    { "1.4.0 Digital",   "2a355aace93ebb99df97bdcc2da664d8c27434b5396adc1266b4753fadc378d7" },
};

static const size_t known_exefs_hashes_count = sizeof(known_exefs_hashes) / sizeof(known_exefs_hashes[0]);
static char g_exefsHash[SHA256_HASH_SIZE * 2 + 1] = {0};
static void print_progress_bar(u8 percent) {
    char bar[31];
    u32 filled = (percent * 30) / 100;

    for (u32 i = 0; i < 30; i++) bar[i] = (i < filled) ? '#' : '-';
    bar[30] = '\0';

    consolePrint("\r[%s] %3u%%", bar, percent);
}

// Checksum runs to the end
static bool calc_file_sha256(const char *path, char *out_hex, bool show_progress) {
    const size_t chunk = 0x40000; // 256 KiB
    bool ok = false;
    u8 *buf = malloc(chunk);
    FILE *f = fopen(path, "rb");

    if (buf && f) {
        fseek(f, 0, SEEK_END);
        u64 total = (u64)ftell(f);
        fseek(f, 0, SEEK_SET);
        Sha256Context ctx;
        sha256ContextCreate(&ctx);
        u64 done = 0;
        int last_percent = -1;
        size_t n;

        while ((n = fread(buf, 1, chunk, f)) > 0) {
            sha256ContextUpdate(&ctx, buf, n);
            done += n;

            if (show_progress && total) {
                int percent = (int)((done * 100) / total);
                if (percent != last_percent) {
                    print_progress_bar((u8)percent);
                    last_percent = percent;
                }
            }
        }

        if (!ferror(f)) {
            u8 hash[SHA256_HASH_SIZE];
            sha256ContextGetHash(&ctx, hash);
            for (u32 i = 0; i < SHA256_HASH_SIZE; i++) sprintf(out_hex + (i * 2), "%02x", hash[i]);
            out_hex[SHA256_HASH_SIZE * 2] = '\0';
            ok = true;
        }
    }

    if (f) fclose(f);
    free(buf);
    return ok;
}

// Returns the label of a matching known hash, or NULL.
static const char *find_known_hash_label(const char *hash)
{
    for (size_t i = 0; i < known_exefs_hashes_count; i++) {
        if (!strcmp(hash, known_exefs_hashes[i].hash)) return known_exefs_hashes[i].label; }
    return NULL;
}

//refreshes g_exefsHash for the main menu
static void update_exefs_hash(void) {
    g_exefsHash[0] = '\0';
    if (!utilsCheckIfFileExists(exefs_path)) return;
    if (!calc_file_sha256(exefs_path, g_exefsHash, false)) g_exefsHash[0] = '\0';
}

static void verify_exefs_after_apply(void) {

    char hash[SHA256_HASH_SIZE * 2 + 1] = {0};
    consolePrint("Verifying exefs.nsp\n");

    if (calc_file_sha256(exefs_path, hash, true)) {

        consolePrint("exefs.nsp hash:%s\n", hash);
        const char *label = find_known_hash_label(hash);
        if (label) consolePrint(BOLDCYAN "Matches: %s\n" RESET, label);
        else 
        consolePrint(BOLDGREEN "UNIQUE HASH FOUND! \n" RESET);
        consolePrint(BOLDYELLOW "Please report to https://github.com/Skylanderfree/odyssey-downgrader-v3/issues/3 \n" RESET);
        consolePrint(BOLDYELLOW "If you believe this is an error make an issue on GitHub \n" RESET);
    } else {
        consolePrint(RED "Failed to calculate exefs.nsp checksum\n" RESET);
    }
}

bool do_add_downgrade() {
    consoleClear();

    bool success = false;
    u32 app_count = 0;
    TitleApplicationMetadata **app_metadata = NULL;
    TitleUserApplicationData user_app_data = {0};

    u8 *buf = NULL;

    NcaContext *base_nca_ctx = NULL;

    ThreadSharedData shared_data = {0};
    Thread read_thread = {0}, write_thread = {0};

    app_metadata = titleGetApplicationMetadataEntries(false, &app_count);
    if (!app_metadata || !app_count)
    {
        consolePrint("app metadata failed\n");
        goto cleanup;
    }

    consolePrint("app metadata succeeded\n");

    buf = usbAllocatePageAlignedBuffer(BLOCK_SIZE);
    if (!buf)
    {
        consolePrint("buf failed\n");
        goto cleanup;
    }

    consolePrint("buf succeeded\n");

    base_nca_ctx = calloc(1, sizeof(NcaContext));
    if (!base_nca_ctx)
    {
        consolePrint("base nca ctx buf failed\n");
        goto cleanup;
    }

    consolePrint("base nca ctx buf succeeded\n");

    utilsSleep(1);

    int odysseyIdx = -1;
    for(int i = 0; i < app_count; i++) {
        TitleApplicationMetadata* meta = app_metadata[i];

        if(meta->title_id == 0x0100000000010000ull) {
            odysseyIdx = i;
            break;
        }
    }

    if(odysseyIdx == -1) {
        consolePrint(BOLDRED "Failed to find odyssey installed.\n" RESET);
        goto cleanup;
    }

    if(!titleGetUserApplicationData(app_metadata[odysseyIdx]->title_id, &user_app_data) || !user_app_data.app_info) {
        consolePrint(BOLDYELLOW "Looks like odyssey is not detected\n" RESET);
        consolePrint(BOLDYELLOW "make sure that you have it installed or gamecard inserted\n" RESET);


        goto cleanup;
    }

    u32 program_count = titleGetContentCountByType(user_app_data.app_info, NcmContentType_Program);
    if (!program_count)
    {
        consolePrint("base app has no program ncas!\n");
        goto cleanup;
    }

    u8 program_id_offset = get_program_id_offset(user_app_data.app_info, program_count);
    if (program_id_offset >= program_count)
    {
        goto cleanup;
    }

    consolePrint("selected title:\n%s (%016lX)\n\n", app_metadata[odysseyIdx]->name, app_metadata[odysseyIdx]->title_id + program_id_offset);

    if (!ncaInitializeContext(
      base_nca_ctx,
      user_app_data.app_info->storage_id,
      (user_app_data.app_info->storage_id == NcmStorageId_GameCard ? HashFileSystemPartitionType_Secure : 0),
      &(user_app_data.app_info->meta_key),
      titleGetContentInfoByTypeAndIdOffset(user_app_data.app_info, NcmContentType_Program, program_id_offset),
      NULL
    ))
    {
        consolePrint("nca initialize base ctx failed\n");
        goto cleanup;
    }

    RomFileSystemContext romfs_ctx = {0};
    if (!romfsInitializeContext(&romfs_ctx, &(base_nca_ctx->fs_ctx[1]), NULL))
    {
        consolePrint("romfs initialize ctx failed\n");
        goto cleanup;
    }
    consolePrint("romfs initialize ctx succeeded\n");

    PartitionFileSystemContext exefs_ctx = {0};
    if (!pfsInitializeContext(&exefs_ctx, &(base_nca_ctx->fs_ctx[0])))
    {
        consolePrint("exefs initialize ctx failed\n");
        goto cleanup;
    }
    consolePrint("exefs initialize ctx succeeded\n");

    u64 required_space = romfs_ctx.size + exefs_ctx.size + patch_data_size;
    if (!check_sd_space(required_space))
    {
        goto cleanup;
    }

    shared_data.section_ctx = &(base_nca_ctx->fs_ctx[1]);
    shared_data.total_offset = romfs_ctx.offset;
    shared_data.total_size = romfs_ctx.size;
    shared_data.mode = false;

    shared_data.data = buf;
    shared_data.data_size = 0;
    shared_data.data_written = 0;

    /* Output files (and ConcatenationFiles, when needed) are created by the read thread. */

dump_start:
    consolePrint("creating threads\n");
    utilsCreateThread(&read_thread, read_thread_func, &shared_data, 2);
    utilsCreateThread(&write_thread, write_thread_func, &shared_data, 2);

    u8 prev_time = 0;
    u64 prev_size = 0;
    u8 percent = 0;

    time_t btn_cancel_start_tmr = 0, btn_cancel_end_tmr = 0;
    bool btn_cancel_cur_state = false, btn_cancel_prev_state = false;

    utilsSetLongRunningProcessState(true);

    consolePrint(BOLDYELLOW "hold b to cancel\n\n" RESET);

    time_t start = time(NULL);
    while(shared_data.data_written < shared_data.total_size)
    {
        if (shared_data.read_error || shared_data.write_error) break;
        //see if this can prevent crashes when pressing HOME for applet and title takeover
        if (!appletMainLoop())
        {
            mutexLock(&g_fileMutex);
            shared_data.transfer_cancelled = true;
            mutexUnlock(&g_fileMutex);
            condvarWakeAll(&g_readCondvar);
            condvarWakeAll(&g_writeCondvar);
            break;
        }
        struct tm ts = {0};
        time_t now = time(NULL);
        localtime_r(&now, &ts);

        size_t size = shared_data.data_written;

        utilsScanPads();
        btn_cancel_cur_state = (utilsGetButtonsHeld() & HidNpadButton_B);

        if (btn_cancel_cur_state && btn_cancel_cur_state != btn_cancel_prev_state)
        {
            btn_cancel_start_tmr = now;
        } else
        if (btn_cancel_cur_state && btn_cancel_cur_state == btn_cancel_prev_state)
        {
            btn_cancel_end_tmr = now;
            if ((btn_cancel_end_tmr - btn_cancel_start_tmr) >= 3)
            {
                mutexLock(&g_fileMutex);
                shared_data.transfer_cancelled = true;
                mutexUnlock(&g_fileMutex);
                //Wake these up so it doesn't freeze when holding B
                condvarWakeAll(&g_readCondvar);
                condvarWakeAll(&g_writeCondvar);
                break;
            }
        } else {
            btn_cancel_start_tmr = btn_cancel_end_tmr = 0;
        }

        btn_cancel_prev_state = btn_cancel_cur_state;

        if (prev_time == ts.tm_sec || prev_size == size) continue;

        percent = (u8)((size * 100) / shared_data.total_size);

        prev_time = ts.tm_sec;
        prev_size = size;

        printf("%lu / %lu (%u%%) | Time elapsed: %lu\n", size, shared_data.total_size, percent, (now - start));
        fflush(stdout);
        consoleUpdate(NULL);
    }

    start = (time(NULL) - start);

    consolePrint("\nwaiting for threads to join\n");
    utilsJoinThread(&read_thread);
    consolePrint("read_thread done: %lu\n", time(NULL));
    utilsJoinThread(&write_thread);
    consolePrint("write_thread done: %lu\n", time(NULL));

    utilsSetLongRunningProcessState(false);

    // Both threads are done: it is now safe to close the output file.
    if (shared_data.fd)
    {
        fclose(shared_data.fd);
        shared_data.fd = NULL;
    }

    utilsCommitSdCardFileSystemChanges();

    if (shared_data.read_error || shared_data.write_error || shared_data.transfer_cancelled)
    {
        // Remove the partial output file
        remove_sd_file_any(shared_data.mode ? exefs_path : romfs_path);

        if (shared_data.transfer_cancelled)
        {
            consolePrint(BOLDRED "process cancelled\n" RESET);
        } else {
            consolePrint("i/o error\n");
        }
        goto cleanup;
    }

    consolePrint("process completed in %lu seconds\n", start);

    if(shared_data.mode == false) {
        shared_data.section_ctx = &(base_nca_ctx->fs_ctx[0]);
        shared_data.total_offset = exefs_ctx.offset;
        shared_data.total_size = exefs_ctx.size;
        shared_data.mode = true;

        shared_data.data = buf;
        shared_data.data_size = 0;
        shared_data.data_written = 0;
        goto dump_start;
    }

    if(user_app_data.app_info->storage_id == NcmStorageId_GameCard) {
       consolePrint(BOLDYELLOW "if odyssey doesn't launch, reinsert your gamecard.\n" RESET);
    }
    success = true;
cleanup:
    if (base_nca_ctx) free(base_nca_ctx);

    titleFreeUserApplicationData(&user_app_data);

    if (buf) free(buf);

    if (app_metadata) free(app_metadata);

    return success;
}

void do_add_patch() {
    consoleClear();
    FsFileSystem* fs = utilsGetSdCardFileSystemObject();

    /* May error if file already exists, this is ok. */
    int r = fsFsCreateFile(fs, patch_path + sdmc_prefix_length, patch_data_size, 0);
    if(R_FAILED(r) && r != R_PATH_EXISTS) {
        consolePrint("failed to write patch (%x)\n", r);
        return;
    }

    FsFile f;
    r = fsFsOpenFile(fs, patch_path + sdmc_prefix_length, FsOpenMode_Write, &f);
    if(R_FAILED(r)) {
        consolePrint("failed to write patch (%x)\n", r);
        return;
    }

    r = fsFileWrite(&f, 0, patch_data, patch_data_size, FsWriteOption_Flush);
    if(R_FAILED(r)) {
        consolePrint("failed to write patch (%x)\n", r);
        fsFileClose(&f);
        return;
    }
    fsFileClose(&f);
    utilsCommitSdCardFileSystemChanges();
}
    // Combine downgrade & patch into one action
void do_add_downgrade_patch() {
    consoleClear();
    // If downgrade is cancelled then patch is not applied
    if (!do_add_downgrade())
    {
        consolePrint(RED "downgrade and patch have been cancelled\n" RESET);
        consolePrint("press any button to exit\n");
        utilsWaitForButtonPress(0);
        return;
    }
    do_add_patch();

    consoleClear();
    consolePrint(BOLDGREEN "downgrade and patch have been added\n" RESET);
    verify_exefs_after_apply();
    consolePrint("press any button to exit\n");
    utilsWaitForButtonPress(0);
}

void do_remove_downgrade() {
    consoleClear();

    int r = remove_sd_file_any(romfs_path);
    if(R_FAILED(r)) {
        consolePrint("failed to delete romfs.bin (%x)\n", r);
        return;
    }

    r = remove_sd_file_any(exefs_path);
    if(R_FAILED(r)) {
        consolePrint("failed to delete exefs.nsp (%x)\n", r);
        return;
    }
}

void do_remove_patch() {
    consoleClear();
    FsFileSystem* fs = utilsGetSdCardFileSystemObject();

    int r = fsFsDeleteFile(fs, patch_path + sdmc_prefix_length);
    if(R_FAILED(r) && r != R_PATH_DOESNT_EXIST) {
        consolePrint("failed to delete patch (%x)\n", r);
        return;
    }

    utilsCommitSdCardFileSystemChanges();
}
    // Combine both actions to remove downgrade & patch
void do_remove_downgrade_patch() {
    consoleClear();

    do_remove_downgrade();
    do_remove_patch();

    consoleClear();
    consolePrint("downgrade and patch removed\n");
    consolePrint("press any button to exit\n");
    utilsWaitForButtonPress(0);
}

typedef enum _status_t {
    STATUS_NONE         = 0,
    STATUS_DOWNGRADE    = 1 << 0,
    STATUS_PATCH        = 1 << 1,
} status_t;


static status_t get_status() {
    status_t status = STATUS_NONE;

    // checks if both downgrade & patch are applied
    if(utilsCheckIfFileExists(romfs_path) && 
    utilsCheckIfFileExists(exefs_path) &&
    utilsCheckIfFileExists(patch_path))
    {
        status |= STATUS_DOWNGRADE;
    }

    // refresh the exefs.nsp hash
    update_exefs_hash();

    return status;
}

int main(int argc, char *argv[])
{
    int ret = 0;

    if (!utilsInitializeResources())
    {
        ret = -1;
        goto out;
    }

    /* Configure input. */
    /* Up to 8 different, full controller inputs. */
    /* Individual Joy-Cons not supported. */
    padConfigureInput(8, HidNpadStyleSet_NpadFullCtrl);
    padInitializeWithMask(&g_padState, 0x1000000FFUL);

    consoleInit(NULL);

    utilsCreateDirectoryTree(romfs_path, false);
    utilsCreateDirectoryTree(exefs_path, false);
    utilsCreateDirectoryTree(patch_path, false);

    bool applet_status;
    int selected_idx = 0;

    #define MENU_COUNT (2)

    const char* menu_names[MENU_COUNT] = {
    "Apply downgrade",
    "Remove downgrade",
    };

    const void (*menu_funcs[MENU_COUNT])() = {
    do_add_downgrade_patch,
    do_remove_downgrade_patch,
    };

    status_t status = get_status();

    while((applet_status = appletMainLoop()))
    {
        utilsConsoleClearNoUpdate();
        printf("odyssey downgrade\npress + to exit.\n\n");

        for(u32 i = 0; i < MENU_COUNT; i++)
        {
            printf("%s%s\n", i == selected_idx ? " -> " : "    ", menu_names[i]);
        }
        printf("\n");

        if(status & STATUS_DOWNGRADE) {
            printf(BOLDGREEN "Downgrade is already added.\n" RESET);
        } else {
            printf(RED "Downgrade is not added.\n" RESET);
        }
        if (g_exefsHash[0])
        {
            const char *label = find_known_hash_label(g_exefsHash);
            printf("exefs.nsp sha256:\n%s\n", g_exefsHash);
            if (label) printf(BOLDCYAN "Matches: %s\n" RESET, label);
            else printf(BOLDYELLOW "Unique hash\n" RESET);
        }

        fflush(stdout);
        consoleUpdate(NULL);

        u64 btn_down = 0, btn_held = 0;
        while((applet_status = appletMainLoop()))
        {
            utilsScanPads();
            btn_down = utilsGetButtonsDown();
            btn_held = utilsGetButtonsHeld();
            if (btn_down || btn_held) break;
        }

        if (btn_down & HidNpadButton_A)
        {
            menu_funcs[selected_idx]();
            status = get_status();
            continue;

        } else
        if ((btn_down & HidNpadButton_Down) || (btn_held & (HidNpadButton_StickLDown | HidNpadButton_StickRDown)))
        {
            selected_idx++;

            if (selected_idx >= MENU_COUNT)
            {
                if (btn_down & HidNpadButton_Down)
                {
                    selected_idx = 0;
                } else {
                    selected_idx = (MENU_COUNT - 1);
                }
            }
        } else
        if ((btn_down & HidNpadButton_Up) || (btn_held & (HidNpadButton_StickLUp | HidNpadButton_StickRUp)))
        {
            selected_idx--;

            if (selected_idx < 0)
            {
                if (btn_down & HidNpadButton_Up)
                {
                    selected_idx = (MENU_COUNT - 1);
                } else {
                    selected_idx = 0;
                }
            }
        } else
        if (btn_down & HidNpadButton_Plus)
        {
            break;
        }

        if (btn_held & (HidNpadButton_StickLDown | HidNpadButton_StickRDown | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) svcSleepThread(50000000); // 50 ms
    }

out:
    utilsCloseResources();

    consoleExit(NULL);

    return ret;
}