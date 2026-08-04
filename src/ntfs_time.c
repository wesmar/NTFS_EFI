/**
 * ntfs_time.c - NTFS (100 ns ticks since 1601-01-01) -> EFI_TIME conversion.
 */

#include "ntfs.h"

/* 100 ns intervals between 1601-01-01 and 1970-01-01 */
#define NTFS_EPOCH_DELTA  116444736000000000ULL

VOID
NtfsEfiConvertTime (
    IN  UINT64    NtfsTime,
    OUT EFI_TIME *EfiTime
    )
{
    UINT64 UnixSec, Rem;
    UINT32 Year, Month, Day, Hour, Minute, Second;
    UINT32 DaysInYear, DaysInMonth;
    /* days per month in a non-leap year */
    UINT8  DPM[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    BOOLEAN Leap;
    UINT32 Days;

    ZeroMem (EfiTime, sizeof (EFI_TIME));
    if (NtfsTime < NTFS_EPOCH_DELTA) return;

    Rem        = (NtfsTime - NTFS_EPOCH_DELTA);
    EfiTime->Nanosecond = (UINT32)((Rem % 10000000ULL) * 100ULL);
    UnixSec    = Rem / 10000000ULL;

    Second     = (UINT32)(UnixSec % 60); UnixSec /= 60;
    Minute     = (UINT32)(UnixSec % 60); UnixSec /= 60;
    Hour       = (UINT32)(UnixSec % 24); UnixSec /= 24;

    /* Days since 1970-01-01 */
    Days = (UINT32)UnixSec;
    Year = 1970;
    for (;;) {
        Leap = ((Year % 4 == 0 && Year % 100 != 0) || (Year % 400 == 0));
        DaysInYear = Leap ? 366 : 365;
        if (Days < DaysInYear) break;
        Days -= DaysInYear;
        Year++;
    }
    Leap = ((Year % 4 == 0 && Year % 100 != 0) || (Year % 400 == 0));
    DPM[1] = Leap ? 29 : 28;
    Month  = 0;
    for (;;) {
        DaysInMonth = DPM[Month];
        if (Days < DaysInMonth) break;
        Days -= DaysInMonth;
        Month++;
    }
    Day = Days + 1;

    EfiTime->Year   = (UINT16)Year;
    EfiTime->Month  = (UINT8)(Month + 1);
    EfiTime->Day    = (UINT8)Day;
    EfiTime->Hour   = (UINT8)Hour;
    EfiTime->Minute = (UINT8)Minute;
    EfiTime->Second = (UINT8)Second;
    EfiTime->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
}

/*
 * Inverse of NtfsEfiConvertTime(): EFI_TIME -> NTFS 100ns ticks since
 * 1601-01-01. Days-since-epoch via Howard Hinnant's days_from_civil
 * algorithm (closed-form, no year/month loop, correct for the whole
 * proleptic Gregorian range EFI_TIME can express).
 */
UINT64
NtfsEfiConvertTimeToNtfs (
    IN CONST EFI_TIME *EfiTime
    )
{
    INT64  Y = (INT64)EfiTime->Year;
    INT64  M = (INT64)EfiTime->Month;
    INT64  D = (INT64)EfiTime->Day;
    INT64  Era, Yoe, Doy, Doe, DaysSinceEpoch;
    UINT64 UnixSec;

    Y -= (M <= 2) ? 1 : 0;
    Era = (Y >= 0 ? Y : Y - 399) / 400;
    Yoe = Y - Era * 400;                                          /* [0, 399]   */
    Doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;          /* [0, 365]   */
    Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;                   /* [0, 146096] */
    DaysSinceEpoch = Era * 146097 + Doe - 719468;

    UnixSec = (UINT64)DaysSinceEpoch * 86400ULL
            + (UINT64)EfiTime->Hour   * 3600ULL
            + (UINT64)EfiTime->Minute * 60ULL
            + (UINT64)EfiTime->Second;

    return UnixSec * 10000000ULL + (EfiTime->Nanosecond / 100ULL) + NTFS_EPOCH_DELTA;
}
