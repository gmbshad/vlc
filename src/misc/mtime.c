/*****************************************************************************
 * mtime.c: high resolution time management functions
 * Functions are prototyped in mtime.h.
 *****************************************************************************
 * Copyright (C) 1998-2004 the VideoLAN team
 * $Id$
 *
 * Authors: Vincent Seguin <seguin@via.ecp.fr>
 *          Rémi Denis-Courmont <rem$videolan,org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#include <vlc/vlc.h>

#include <stdio.h>                                              /* sprintf() */
#include <time.h>                      /* clock_gettime(), clock_nanosleep() */
#include <stdlib.h>                                               /* lldiv() */


#if defined( PTH_INIT_IN_PTH_H )                                  /* GNU Pth */
#   include <pth.h>
#endif

#ifdef HAVE_UNISTD_H
#   include <unistd.h>                                           /* select() */
#endif

#ifdef HAVE_KERNEL_OS_H
#   include <kernel/OS.h>
#endif

#if defined( WIN32 ) || defined( UNDER_CE )
#   include <windows.h>
#else
#   include <sys/time.h>
#endif

#if defined(HAVE_NANOSLEEP) && !defined(HAVE_STRUCT_TIMESPEC)
struct timespec
{
    time_t  tv_sec;
    int32_t tv_nsec;
};
#endif

#if defined(HAVE_NANOSLEEP) && !defined(HAVE_DECL_NANOSLEEP)
int nanosleep(struct timespec *, struct timespec *);
#endif

/**
 * Return a date in a readable format
 *
 * This function converts a mtime date into a string.
 * psz_buffer should be a buffer long enough to store the formatted
 * date.
 * \param date to be converted
 * \param psz_buffer should be a buffer at least MSTRTIME_MAX_SIZE characters
 * \return psz_buffer is returned so this can be used as printf parameter.
 */
char *mstrtime( char *psz_buffer, mtime_t date )
{
    static mtime_t ll1000 = 1000, ll60 = 60, ll24 = 24;

    snprintf( psz_buffer, MSTRTIME_MAX_SIZE, "%02d:%02d:%02d-%03d.%03d",
             (int) (date / (ll1000 * ll1000 * ll60 * ll60) % ll24),
             (int) (date / (ll1000 * ll1000 * ll60) % ll60),
             (int) (date / (ll1000 * ll1000) % ll60),
             (int) (date / ll1000 % ll1000),
             (int) (date % ll1000) );
    return( psz_buffer );
}

/**
 * Convert seconds to a time in the format h:mm:ss.
 *
 * This function is provided for any interface function which need to print a
 * time string in the format h:mm:ss
 * date.
 * \param secs  the date to be converted
 * \param psz_buffer should be a buffer at least MSTRTIME_MAX_SIZE characters
 * \return psz_buffer is returned so this can be used as printf parameter.
 */
char *secstotimestr( char *psz_buffer, int i_seconds )
{
    snprintf( psz_buffer, MSTRTIME_MAX_SIZE, "%d:%2.2d:%2.2d",
              (int) (i_seconds / (60 *60)),
              (int) ((i_seconds / 60) % 60),
              (int) (i_seconds % 60) );
    return( psz_buffer );
}


/**
 * Return high precision date
 *
 * Uses the gettimeofday() function when possible (1 MHz resolution) or the
 * ftime() function (1 kHz resolution).
 */
mtime_t mdate( void )
{
#if defined (HAVE_CLOCK_NANOSLEEP)
    struct timespec ts;

# if (_POSIX_MONOTONIC_CLOCK - 0 >= 0)
    /* Try to use POSIX monotonic clock if available */
    if( clock_gettime( CLOCK_MONOTONIC, &ts ) )
# endif
        /* Run-time fallback to real-time clock (always available) */
        (void)clock_gettime( CLOCK_REALTIME, &ts );

    return ((mtime_t)ts.tv_sec * (mtime_t)1000000)
           + (mtime_t)(ts.tv_nsec / 1000);

#elif defined( HAVE_KERNEL_OS_H )
    return( real_time_clock_usecs() );

#elif defined( WIN32 ) || defined( UNDER_CE )
    /* We don't need the real date, just the value of a high precision timer */
    static mtime_t freq = I64C(-1);
    mtime_t usec_time;

    if( freq == I64C(-1) )
    {
        /* Extract from the Tcl source code:
         * (http://www.cs.man.ac.uk/fellowsd-bin/TIP/7.html)
         *
         * Some hardware abstraction layers use the CPU clock
         * in place of the real-time clock as a performance counter
         * reference.  This results in:
         *    - inconsistent results among the processors on
         *      multi-processor systems.
         *    - unpredictable changes in performance counter frequency
         *      on "gearshift" processors such as Transmeta and
         *      SpeedStep.
         * There seems to be no way to test whether the performance
         * counter is reliable, but a useful heuristic is that
         * if its frequency is 1.193182 MHz or 3.579545 MHz, it's
         * derived from a colorburst crystal and is therefore
         * the RTC rather than the TSC.  If it's anything else, we
         * presume that the performance counter is unreliable.
         */
        LARGE_INTEGER buf;

        freq = ( QueryPerformanceFrequency( &buf ) &&
                 (freq == I64C(1193182) || freq == I64C(3579545) ) )
               ? buf : 0;
    }

    if( freq != 0 )
    {
        /* Microsecond resolution */
        QueryPerformanceCounter( (LARGE_INTEGER *)&usec_time );
        return ( usec_time * 1000000 ) / freq;
    }
    else
    {
        /* Fallback on GetTickCount() which has a milisecond resolution
         * (actually, best case is about 10 ms resolution)
         * GetTickCount() only returns a DWORD thus will wrap after
         * about 49.7 days so we try to detect the wrapping. */

        static CRITICAL_SECTION date_lock;
        static mtime_t i_previous_time = I64C(-1);
        static int i_wrap_counts = -1;

        if( i_wrap_counts == -1 )
        {
            /* Initialization */
            i_previous_time = I64C(1000) * GetTickCount();
            InitializeCriticalSection( &date_lock );
            i_wrap_counts = 0;
        }

        EnterCriticalSection( &date_lock );
        usec_time = I64C(1000) *
            (i_wrap_counts * I64C(0x100000000) + GetTickCount());
        if( i_previous_time > usec_time )
        {
            /* Counter wrapped */
            i_wrap_counts++;
            usec_time += I64C(0x100000000000);
        }
        i_previous_time = usec_time;
        LeaveCriticalSection( &date_lock );

        return usec_time;
    }
#else
    struct timeval tv_date;

    /* gettimeofday() cannot fail given &tv_date is a valid address */
    (void)gettimeofday( &tv_date, NULL );
    return( (mtime_t) tv_date.tv_sec * 1000000 + (mtime_t) tv_date.tv_usec );
#endif
}

/**
 * Wait for a date
 *
 * This function uses select() and an system date function to wake up at a
 * precise date. It should be used for process synchronization. If current date
 * is posterior to wished date, the function returns immediately.
 * \param date The date to wake up at
 */
void mwait( mtime_t date )
{
#if defined (HAVE_CLOCK_NANOSLEEP)
    lldiv_t d = lldiv( date, 1000000 );
    struct timespec ts = { d.quot, d.rem * 1000 };

# if (_POSIX_MONOTONIC_CLOCK - 0 >= 0)
    if( clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL ) )
# endif
        clock_nanosleep( CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL );
#else

    mtime_t usec_time, delay;

    usec_time = mdate();
    delay = date - usec_time;
    if( delay > 0 )
        msleep( delay );


    struct timeval tv_date;
    mtime_t        delay;          /* delay in msec, signed to detect errors */

    /* see mdate() about gettimeofday() possible errors */
    gettimeofday( &tv_date, NULL );

    /* calculate delay and check if current date is before wished date */
    delay = date - (mtime_t) tv_date.tv_sec * 1000000
                 - (mtime_t) tv_date.tv_usec
                 - 10000;

    if( delay > 0 )
        msleep( delay );
#endif
}

/**
 * More precise sleep()
 *
 * Portable usleep() function.
 * \param delay the amount of time to sleep
 */
void msleep( mtime_t delay )
{
#if defined( HAVE_CLOCK_NANOSLEEP ) 
    lldiv_t d = lldiv( delay, 1000000 );
    struct timespec ts = { d.quot, d.rem * 1000 };

# if (_POSIX_MONOTONIC_CLOCK - 0 >= 0)
    if( clock_nanosleep( CLOCK_MONOTONIC, 0, &ts, NULL ) )
# endif
        clock_nanosleep( CLOCK_REALTIME, 0, &ts, NULL );

#elif defined( HAVE_KERNEL_OS_H )
    snooze( delay );

#elif defined( PTH_INIT_IN_PTH_H )
    pth_usleep( delay );

#elif defined( ST_INIT_IN_ST_H )
    st_usleep( delay );

#elif defined( WIN32 ) || defined( UNDER_CE )
    Sleep( (int) (delay / 1000) );

#elif defined( HAVE_NANOSLEEP )
    struct timespec ts_delay;

    ts_delay.tv_sec = delay / 1000000;
    ts_delay.tv_nsec = (delay % 1000000) * 1000;

    nanosleep( &ts_delay, NULL );

#else
    struct timeval tv_delay;

    tv_delay.tv_sec = delay / 1000000;
    tv_delay.tv_usec = delay % 1000000;

    /* select() return value should be tested, since several possible errors
     * can occur. However, they should only happen in very particular occasions
     * (i.e. when a signal is sent to the thread, or when memory is full), and
     * can be ignored. */
    select( 0, NULL, NULL, NULL, &tv_delay );
#endif
}

/*
 * Date management (internal and external)
 */

/**
 * Initialize a date_t.
 *
 * \param date to initialize
 * \param divider (sample rate) numerator
 * \param divider (sample rate) denominator
 */

void date_Init( date_t *p_date, uint32_t i_divider_n, uint32_t i_divider_d )
{
    p_date->date = 0;
    p_date->i_divider_num = i_divider_n;
    p_date->i_divider_den = i_divider_d;
    p_date->i_remainder = 0;
}

/**
 * Change a date_t.
 *
 * \param date to change
 * \param divider (sample rate) numerator
 * \param divider (sample rate) denominator
 */

void date_Change( date_t *p_date, uint32_t i_divider_n, uint32_t i_divider_d )
{
    p_date->i_divider_num = i_divider_n;
    p_date->i_divider_den = i_divider_d;
}

/**
 * Set the date value of a date_t.
 *
 * \param date to set
 * \param date value
 */
void date_Set( date_t *p_date, mtime_t i_new_date )
{
    p_date->date = i_new_date;
    p_date->i_remainder = 0;
}

/**
 * Get the date of a date_t
 *
 * \param date to get
 * \return date value
 */
mtime_t date_Get( const date_t *p_date )
{
    return p_date->date;
}

/**
 * Move forwards or backwards the date of a date_t.
 *
 * \param date to move
 * \param difference value
 */
void date_Move( date_t *p_date, mtime_t i_difference )
{
    p_date->date += i_difference;
}

/**
 * Increment the date and return the result, taking into account
 * rounding errors.
 *
 * \param date to increment
 * \param incrementation in number of samples
 * \return date value
 */
mtime_t date_Increment( date_t *p_date, uint32_t i_nb_samples )
{
    mtime_t i_dividend = (mtime_t)i_nb_samples * 1000000;
    p_date->date += i_dividend / p_date->i_divider_num * p_date->i_divider_den;
    p_date->i_remainder += (int)(i_dividend % p_date->i_divider_num);

    if( p_date->i_remainder >= p_date->i_divider_num )
    {
        /* This is Bresenham algorithm. */
        p_date->date += p_date->i_divider_den;
        p_date->i_remainder -= p_date->i_divider_num;
    }

    return p_date->date;
}
