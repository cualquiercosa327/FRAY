#include "dvd.h"

#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#include <gcutil.h>
#include <ogc/cache.h>
#include <ogc/irq.h>
#include <ogc/lwp.h>

#define GET_OFFSET(o) ((u32)((o[0] << 16) | (o[1] << 8) | o[2]))

static dvdcbcallback cb = NULL; //r13_43E4
static u32 status = 0; //r13_43E8
static u32 total_entries = 0; //r13_441C
static char* string_table = NULL; //r13_4420
static FSTEntry* entry_table = NULL; //r13_4424
static u32* start_memory = NULL; //r13_4428
static lwpq_t dvd_wait_queue;

static s8* fst;

static void __DVDFSInit(void)
{
    dvdcmdblk cmdblk;
    DiskHeader header;

    memset(&cmdblk, 0, sizeof(dvdcmdblk));
    memset(&header, 0, sizeof(DiskHeader));

    start_memory = (u32*)(0x80000000);

    DVD_ReadPrio(&cmdblk, &header, sizeof(DiskHeader), 0, 2); //Read in the disc header

    if (header.DVDMagicWord != 0xC2339F3D) {
        //We'll do some error handling for this later
    }

    fst = memalign(32, header.FSTSize);
    start_memory[14] = header.FSTOffset;
    start_memory[15] = header.FSTSize;

    DCFlushRange((void*)0x80000000, 0x40);

    if (header.FSTSize > 0x4000) {
        u32 size = header.FSTSize;
        for (u32 i = 0; size > 0; i++) {
            memset(&cmdblk, 0, sizeof(dvdcmdblk));
            if (size > 0x4000) {
                DVD_ReadPrio(&cmdblk, fst + (i * 0x4000), 0x4000, (header.FSTOffset + (0x4000 * i)), 2);
                size -= 0x4000;
            } else {
                DVD_ReadPrio(&cmdblk, fst + (i * 0x4000), size, (header.FSTOffset + (0x4000 * i)), 2);
                size = 0;
            }
        }
    }

    entry_table = (FSTEntry*)fst;
    total_entries = entry_table[0].len;
    string_table = (char*)&(entry_table[total_entries]);
}

void DVDInit(void)
{
    DVD_Init();
    DVD_Mount();
    __DVDFSInit();
    LWP_InitQueue(&dvd_wait_queue);
}

//800195D0
void DVD_CheckDisk(void)
{
    //DVD_DisplayDiskError(resetCallback);
    //MemoryCard_CheckToSave();
}

BOOL DVDFastOpen(s32 entrynum, dvdfileinfo* fileinfo)
{
    if (entrynum >= 0 && entrynum < total_entries) {
        u8 filetype = entry_table[entrynum].filetype;
        if (filetype == T_FILE) {
            fileinfo->addr = entry_table[entrynum].addr;
            fileinfo->len = entry_table[entrynum].len;
            fileinfo->cb = NULL;
            fileinfo->block.state = 0;
            return TRUE;
        }
    }
    return FALSE;
}

static void cbForCancelAsync(s32 result, dvdcmdblk* cmd)
{
    LWP_ThreadBroadcast(dvd_wait_queue);
}

BOOL DVDCancelAsync(dvdcmdblk* cmd, dvdcbcallback callback)
{
    u32 intr = IRQ_Disable();
    switch (cmd->state) {
    case DVD_STATE_BUSY: {

    } break;

    case DVD_STATE_WAITING: {
        if (cmd->cb != NULL) {
            (*cmd->cb)(0, cmd);
        }
    } break;

    case 3: {
        switch (cmd->cmd) {
        case 4:
        case 5:
        case 13:
        case 15: {
            if (callback != NULL) {
                (*callback)(0, cmd);
            }
        }
            if (status != 0) {
                IRQ_Restore(intr);
                return 0;
            }
            cb = callback;
            status = 1;
            break;
        }
    } break;

    case DVD_STATE_NO_DISK:
    case DVD_STATE_COVER_OPEN:
    case DVD_STATE_WRONG_DISK:
    case DVD_STATE_MOTOR_STOPPED:
    case DVD_STATE_RETRY: {
        switch (cmd->state) {
        case 4:
            status = 3;
            break;

        case 5:
            status = 4;
            break;

        case 6:
            status = 1;
            break;

        case 7:
            status = 7;
            break;

        case 11:
            status = 1;
            break;
        }
        cmd->state = 10;
        if (cmd->cb != NULL) {
            (*cmd->cb)(DVD_ERROR_CANCELED, cmd);
        }
        if (callback != NULL) {
            (*callback)(0, cmd);
        }
    } break;

    case DVD_STATE_FATAL_ERROR:
    case DVD_STATE_END:
    case DVD_STATE_CANCELED: {
        if (callback != NULL) {
            (*callback)(DVD_ERROR_OK, cmd);
        }
    } break;
    }
    IRQ_Restore(intr);
    return TRUE;
}

s32 DVDCancel(dvdcmdblk* cmd)
{
    if (DVDCancelAsync(cmd, cbForCancelAsync) == FALSE) {
        return -1;
    } else {
        u32 intr = IRQ_Disable();
        while (true) {
            s32 state = cmd->state;
            if (1 >= (state + 1) || state == DVD_STATE_CANCELED || state == DVD_STATE_COVER_CLOSED) {
                break;
            }
            u32 command = cmd->cmd;
            if (1 >= (command - 4) || command == 13 || command == 15) {
                break;
            }
            LWP_ThreadSleep(dvd_wait_queue);
        }
        IRQ_Restore(intr);
        return 0;
    }
}

BOOL DVDClose(dvdfileinfo* fileinfo)
{
    //DVDCancel(&fileinf->cmd);
    DVD_Reset(DVD_RESETSOFT);
    return TRUE;
}

s32 DVDConvertFilenameToEntrynum(char* filename)
{
    FSTEntry* p = entry_table;
    for (u32 i = 1; i < total_entries; ++i) { //Start @ 1 to skip FST header
        u32 string_offset = GET_OFFSET(p[i].offset);
        u32 string = (u32)string_table + string_offset;
        if (strcmp((char*)string, filename) == 0) {
            return i;
        }
    }
    return -1;
}

s32 DVDConvertPathToEntrynum(char* filepath)
{
    if (filepath == NULL) {
        return -1;
    }
    char* file = NULL;

    char* dir = strtok(filepath, "/");
    while (dir != NULL) {
        file = dir;
        dir = strtok(NULL, "/");
    }
    return DVDConvertFilenameToEntrynum(file);
}
