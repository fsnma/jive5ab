// structs in this files are used to define memory layout of the disk info 
// in the user directory

#ifndef JIVE5A_USERDIR_LAYOUT_H
#define JIVE5A_USERDIR_LAYOUT_H

#include <xlrdefines.h>
#include <ezexcept.h>
#include <scan.h>
#include <evlbidebug.h>

#include <stdint.h> // for [u]int<N>_t  types
#include <string.h> // for strncpy
#include <string>
#include <limits>

DECLARE_EZEXCEPT(userdirexception)

// The original scandirectory
template<unsigned int Maxscans> struct ScanDir {
    // The number of recorded scans
    unsigned int nScans( void ) const {
        if( nRecordedScans<0 )
            THROW_EZEXCEPT(userdirexception, "Negative number of recorded scans?!");
        // Ok, nRecordedScans is non-negative so cast to unsigned int is safe
        return (unsigned int)nRecordedScans;
    }

    // Get access to scans. Detects if you're trying to
    // access outside the recorded scans.
    ROScanPointer getScan( unsigned int scan ) const {
        // Check if there *are* recorded scans && the requested one
        // is within the recorded range
        if( nRecordedScans>0 /* integer comparison */ &&
            scan<(unsigned int)nRecordedScans /* unsigned, but is safe! */ ) 
            return ROScanPointer(from_c_str(&scanName[scan][0], Maxlength), 
                                 scanStart[scan], scanLength[scan], scan);
        // Otherwise ...
        THROW_EZEXCEPT(userdirexception, "requested scan (#" << scan << ") out-of-range: "
                       << " nRecorded=" << nRecordedScans);  
    }

    // If you want to record a new scan, use this method
    // to get the next writable entry and do *not* forget
    // the values you got. The system will internally
    // bump the nRecordedScans as soon as you call this
    // one. The "ScanPointer" you got will be the only
    // writable entry to the scan.
    ScanPointer getNextScan( void ) {
        // Assert sanity of current state.
        // That is: check if there's room for a new
        // recorded scan.
        // nRecorded < 0 => internal state bollox0red
        // nRecorded must also be < Maxscans, otherwise
        // there's no room for a new scan
        if( nRecordedScans<0 )
            THROW_EZEXCEPT(userdirexception, "nRecordedScans<0 makes no sense!");
        if( (unsigned int)nRecordedScans>=Maxscans )
            THROW_EZEXCEPT(userdirexception, "scanDirectory is full!");

        // Allocate the new Scan
        return ScanPointer( nRecordedScans++ );
    }

    void setScan( const ScanPointer& scan )  {
        // some sanity checks
        if( (int)scan.index() >= nRecordedScans )
            THROW_EZEXCEPT(userdirexception, "scan index larger than number of recorded scans!");
        if ( scan.name().size() >= Maxlength ) 
            THROW_EZEXCEPT(userdirexception, "scan name too long!");

        strncpy(&scanName[scan.index()][0], scan.name().c_str(), Maxlength);
        scanStart[scan.index()] = scan.start();
        scanLength[scan.index()] = scan.length();

        recordPointer( scan.start() + scan.length() );
    }

    uint64_t recordPointer( void ) const {
        return _recordPointer;
    }

    void recordPointer( uint64_t newrecptr ) {
        _recordPointer = newrecptr;
    }

    uint64_t playPointer( void ) const {
        return _playPointer;
    }

    void playPointer( uint64_t newpp ) {
        _playPointer =  newpp;
    }

    double playRate( void ) const {
        return _playRate;
    }

    void playRate( double newpr ) {
        _playRate = newpr;
    }

    void clear_scans( void ) {
        nRecordedScans = 0;
    }

    void clear( void ) {
        memset( this, 0, sizeof(*this) );
    }

    void remove_last_scan( void ) {
        if ( nRecordedScans <= 0 ) {
            THROW_EZEXCEPT(userdirexception, "no scan to remove in scanDir");
        }
        nRecordedScans--;
    }

    unsigned int insanityFactor( void ) const {
        unsigned int res = 0;
        res += (((nRecordedScans < 0) || ((unsigned int)nRecordedScans > Maxscans)) ? 1 : 0);
        res +=  (((nextScan < 0) || ((unsigned int)nextScan >= Maxscans)) ? 1 : 0);
        
        // check the scans for inconsistencies
        uint64_t expectedRecordPointer = 0;
        int scans = std::min( nRecordedScans, (int)Maxscans );
        for ( unsigned int i = 0; i < (unsigned int)std::max( scans, 0 ); i++ ) {
            res += ( (scanStart[i] != expectedRecordPointer) ? 1 : 0 );
            res += ( (scanName[i][0] == '\0') ? 1 : 0 );
            expectedRecordPointer = scanStart[i] + scanLength[i];
        }
        res += ( (_recordPointer != expectedRecordPointer) ? 1 : 0 );

        return res;
    }

    // hmmm ...
    // this is to sanitize. If it detects possible fishyness
    // (nRecordedScans<0, nextScan<0, nRecordedScans>maxscans,
    // nextScan>=maxscans ) it resets itself to "empty".
    // Needed for fixing after reading this from StreamStor - 
    // you never can be *really* sure that you read a ScanDir
    // - maybe you read some data. This is how it's done in Mark5A.
    // And we should try to mimick the behaviour
    void sanitize( void ) {
        if( nRecordedScans<0 || (unsigned int)nRecordedScans>Maxscans ||
            nextScan<0 || (unsigned int)nextScan>=Maxscans ) {
            ::memset(this, 0x0, sizeof(ScanDir));
            DEBUG(0, "Detected fishiness in 'sanitize'. Cleaning out ScanDir!" << std::endl);
        }
    }

    // the streamstor has a method XLRRecoverData, which can be used
    // to recover data after various failure modes
    // recover will try to restore the ScanDir
    void recover( uint64_t recovered_record_pointer ) {
        if ( recovered_record_pointer == 0 ) {
            nRecordedScans = 0;
            return;
        }
        _recordPointer = recovered_record_pointer;
        int last_scan = (int)nRecordedScans - 1;
        if ( last_scan >= 0 ) {

            while ( (last_scan >= 0) && 
                    (scanStart[last_scan] >= recovered_record_pointer) ) {
                last_scan--;
                nRecordedScans--;
            }
            if ( last_scan >= 0 ) {
                scanLength[last_scan] = recovered_record_pointer - scanStart[last_scan];
            }
        }
        else {
            scanStart[0] = 0;
            scanLength[0] = recovered_record_pointer;
            strncpy(&scanName[0][0], "recovered scan", Maxlength);
            nRecordedScans++;
        }
    }

    private:
        ScanDir();

        // Actual # of recorded scans
        int                nRecordedScans;
        // "pointer" to next_scan for "next_scan"?
        int                nextScan;
        // Arrays of scannames, startpos + lengths
        char               scanName[Maxscans][Maxlength];
        uint64_t           scanStart[Maxscans]; /* Start byte position */ 
        uint64_t           scanLength[Maxscans]; /* Length in bytes */ 
        // Current record and playback pointers
        uint64_t           _recordPointer;
        uint64_t           _playPointer;
        double             _playRate;
};

// "Enhanced Mark5 module directory" documented in:
// http://www.haystack.mit.edu/tech/vlbi/mark5/mark5_memos/046.pdf
struct EnhancedDirectoryHeader {
    int32_t       directory_version;
    uint32_t      status;
    char          vsn[32];
    char          companion_vsn[32];
    char          continued_to_vsn[32];
    unsigned char spare[24];

    void clear();
};
struct EnhancedDirectoryEntry {
    int32_t       data_type;
    uint32_t      scan_number;
    char          scan_name[32];
    char          experiment[8];
    char          station_code[8];
    uint64_t      start_byte;
    uint64_t      stop_byte;
    unsigned char first_time_tag[8]; // BCD
    uint32_t      first_frame_number;
    uint32_t      byte_offset;
    uint32_t      number_of_frames;
    uint32_t      total_data_rate_mbps;
    uint32_t      track_bitstream_mask;
    unsigned char spare[28];

    void clear();
};

struct SDK8_DRIVEINFO {
    char Model[XLR_MAX_DRIVENAME];
    char Serial[XLR_MAX_DRIVESERIAL];
    char Revision[XLR_MAX_DRIVEREV];
    uint32_t Capacity;
    BOOLEAN SMARTCapable;
    BOOLEAN SMARTState;

    void get( S_DRIVEINFO& out ) const;
    void set( S_DRIVEINFO& in );
};
struct SDK9_DRIVEINFO {
    char Model[XLR_MAX_DRIVENAME];
    char Serial[XLR_MAX_DRIVESERIAL];
    char Revision[XLR_MAX_DRIVEREV];
    uint64_t Capacity;
    BOOLEAN SMARTCapable;
    BOOLEAN SMARTState;

    void get( S_DRIVEINFO& out ) const;
    void set( S_DRIVEINFO& in );
};

template<unsigned int nDisks, typename DriveInfo, bool> struct DiskInfoCacheMembers { 
    char       actualVSN[VSNLength];
    DriveInfo  driveInfo[nDisks];
};

template<unsigned int nDisks, typename DriveInfo> struct DiskInfoCacheMembers<nDisks, DriveInfo, true> { 
    char       actualVSN[VSNLength];
    DriveInfo  driveInfo[nDisks];
    char       bankBVSN[VSNLength];
};

template <unsigned int nDisks, typename DriveInfo, bool BankB>
struct DiskInfoCache : private DiskInfoCacheMembers<nDisks, DriveInfo, BankB> {
    void clear( void ) {
        memset( this, 0, sizeof(*this) );
    }

    unsigned int insanityFactor( void ) const {
        unsigned int res = 0;
        // we expect for each disk that the Model, Serial, Revision and Capacity
        // are either all zero or all non zero
        for (unsigned int i = 0; i < nDisks; i++) {
            bool expect = (this->driveInfo[i].Capacity == 0 );
            res += ( (((this->driveInfo[i].Model[0] == '\0') != expect) ||
                      ((this->driveInfo[i].Serial[0] == '\0') != expect) ||
                      ((this->driveInfo[i].Revision[0] == '\0') != expect)) 
                     ? 1 : 0 );
        }

        return res;
    };

    std::string getVSN( void ) const {
        return from_c_str( this->actualVSN, sizeof(this->actualVSN) );
    }

    void setVSN( std::string& vsn ) {
        ::strncpy( this->actualVSN, vsn.c_str(), VSNLength );
    }

    void getDriveInfo( unsigned int disk, S_DRIVEINFO& out ) const {
        if( disk>=nDisks ) {
            THROW_EZEXCEPT(userdirexception, "requested disk#" << disk << 
                    " out of range (max is " << nDisks << ")");
        }
        this->driveInfo[disk].get( out );
    }
    
    void setDriveInfo( unsigned int disk, S_DRIVEINFO& in ) {
        if( disk>=nDisks ) {
            THROW_EZEXCEPT(userdirexception, "requested disk#" << disk << 
                    " out of range (max is " << nDisks << ")");
        }
        this->driveInfo[disk].set( in );
    }

    unsigned int numberOfDisks() const {
        return nDisks;
    }
private:
    DiskInfoCache();
    
};

#endif