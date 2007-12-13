// macros for debugging
#ifndef EVLBI5A_EVLBIDEBUG_H
#define EVLBI5A_EVLBIDEBUG_H

#include <iostream>
#include <sstream>


#ifdef __GNUC__
#define EVDBG_FUNC "[" << __PRETTY_FUNCTION__ << "] "
#else
#define EVDBG_FUNC ""
#endif



int dbglev_fn( void );  // get current debuglevel
int dbglev_fn( int n ); // set current level to 'n', returns previous level

// functionname printing threshold:
// if dbglev()>fnthres() then the functionnames
// where the DEBUG() was issued from will be printed as well
//  [that is: if the __PRETTY_FUNCTION__ is available otherwise
//   it has no effect]
int fnthres_fn( void ); // get current threshold value
int fnthres_fn( int n );  // set current threshold value, returns previous level

void do_cerr_lock( void );
void do_cerr_unlock( void );

// Prepare the debugstring in a local variable.
// We do that so the amount of time spent holding the lock
// is minimal.
#define DEBUG(a, b) \
    do {\
        if( a<=dbglev_fn() ) {\
            std::ostringstream OsS_ZyP;\
            OsS_ZyP << "<"<< a << "> ";\
            if( dbglev_fn()>fnthres_fn() ) \
                OsS_ZyP << EVDBG_FUNC; \
            OsS_ZyP << b;\
            do_cerr_lock();\
            std::cerr << OsS_ZyP.str();\
            do_cerr_unlock();\
        }\
    } while( 0 );



// Nice ones: temporary change the level, that is:
// for the lifetime of these objects. Constructor
// aters the level and saves the old value, destructor
// puts the old level back.
// Well.. not just now :)



#endif