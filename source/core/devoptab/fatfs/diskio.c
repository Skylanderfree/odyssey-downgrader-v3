/*-----------------------------------------------------------------------*/
/* Low level disk I/O module SKELETON for FatFs     (C)ChaN, 2019        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include <core/nxdt_utils.h>
#include <core/bis_storage.h>

#include <core/devoptab/fatfs/ff.h>		/* Obtains integer types */
#include <core/devoptab/fatfs/diskio.h>	/* Declarations of disk functions */

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                      */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (
    BYTE pdrv		/* Physical drive number to identify the drive */
)
{
    NX_IGNORE_ARG(pdrv);
    return 0;
}

/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize (
    BYTE pdrv				/* Physical drive number to identify the drive */
)
{
    NX_IGNORE_ARG(pdrv);
    return 0;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (
    BYTE pdrv,		/* Physical drive number to identify the drive */
    BYTE *buff,		/* Data buffer to store read data */
    LBA_t sector,	/* Start sector in LBA */
    UINT count		/* Number of sectors to read */
)
{
    FsStorage *bis_storage = NULL;
    u64 offset = 0, size = 0;
    Result rc = 0;
    DRESULT ret = RES_PARERR;

    bisStorageControlMutex(true);

    /* Get pointer to FsStorage object. */
    bis_storage = bisStorageGetFsStorageByFatFsDriveNumber(pdrv);
    if (!bis_storage)
    {
        LOG_MSG_ERROR("Failed to retrieve FsStorage object for drive number %u!", pdrv);
        goto end;
    }

    /* Calculate data offset and size. */
    offset = ((u64)FF_MAX_SS * (u64)sector);
    size = ((u64)FF_MAX_SS * (u64)count);

    /* Read BIS storage. */
    rc = fsStorageRead(bis_storage, (s64)offset, buff, size);
    ret = (R_SUCCEEDED(rc) ? RES_OK : RES_ERROR);
    if (ret == RES_ERROR) LOG_MSG_ERROR("Failed to read 0x%lX-byte long block at offset 0x%lX from drive number %u!", offset, size, pdrv);

end:
    bisStorageControlMutex(false);

    return ret;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0

DRESULT disk_write (
    BYTE pdrv,			/* Physical drive number to identify the drive */
    const BYTE *buff,	/* Data to be written */
    LBA_t sector,		/* Start sector in LBA */
    UINT count			/* Number of sectors to write */
)
{
    NX_IGNORE_ARG(pdrv);
    NX_IGNORE_ARG(buff);
    NX_IGNORE_ARG(sector);
    NX_IGNORE_ARG(count);
    return RES_OK;
}

#endif

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (
    BYTE pdrv,		/* Physical drive nmuber (0..) */
    BYTE cmd,		/* Control code */
    void *buff		/* Buffer to send/receive control data */
)
{
    NX_IGNORE_ARG(pdrv);
    NX_IGNORE_ARG(cmd);
    NX_IGNORE_ARG(buff);
    return RES_OK;
}
