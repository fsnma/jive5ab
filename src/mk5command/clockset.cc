// Copyright (C) 2007-2013 Harro Verkouter
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
// 
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE.  See the GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
// 
// Author:  Harro Verkouter - verkouter@jive.nl
//          Joint Institute for VLBI in Europe
//          P.O. Box 2
//          7990 AA Dwingeloo
#include <mk5_exception.h>
#include <mk5command/mk5.h>
#include <streamutil.h>
#include <iostream>

using namespace std;


// Mark5BDIM clock_set (replaces 'play_rate')
string clock_set_fn(bool qry, const vector<string>& args, runtime& rte ) {
    ostringstream       reply;
    const transfer_type ctm( rte.transfermode );
    mk5b_inputmode_type curipm;

    reply << "!" << args[0] << (qry?('?'):('=')) << " ";

    // Query may execute always, command only when not doing anything
    // with the i/o board
    INPROGRESS(rte, reply, !qry && (toio(ctm) || fromio(ctm)))

    // Get current inputmode - some values should
    // not be overwritten with mk5b default ones
    // (mostly the booleans - difficult to have
    //  "sentinel" values to indicate "do not change")
    rte.get_input( curipm );

    if( qry ) {
        samplerate_type        rate = curipm.clockfreq/1000000;
        mk5bdom_inputmode_type magicmode( mk5bdom_inputmode_type::empty );

        // If 'magic mode' is configured, yield different output
        rte.get_input( magicmode );
        if( magicmode.mode.empty()==false ) {
            reply << "0 : " << format("%.3lf", boost::rational_cast<double>(rte.trackbitrate())) << " ;";
            return reply.str();
        }
        // No 'magic mode' - carry on with hardware results

        // Get the 'K' registervalue: f = 2^(k+1)
        // Go from e^(k+1) => 2^(k+1)
        const double clkfreq = ::exp( ((double)(curipm.k+1))*M_LN2 );

        reply << "0 : " << clkfreq 
              << " : " << ((curipm.selcgclk)?("int"):("ext"))
              << " : " << rate << " ;";
        return reply.str();
    }

    // if command, we require two non-empty arguments.
    // clock_set = <clock freq> : <clock source> [: <clock-generator-frequency>]
    //
    // HV: 24-sep-2013 The clock generator frequency should only 
    //                 be allowed to be set if <clock source>
    //                 is "int"(ernal). Otherwise we ignore the
    //                 supplied frequency
    if( args.size()<3 ||
        args[1].empty() || args[2].empty() ) {
        reply << "8 : must have at least two non-empty arguments ; ";
        return reply.str();
    }

    // Verify we recognize the clock-source
    ASSERT2_COND( args[2]=="int"||args[2]=="ext",
                  SCINFO(" clock-source " << args[2] << " unknown, use int or ext") );

    // We already got the current input mode.
    //
    // Modify it such that it reflects the new clock settings.
    // I.e. mark values that shouldn't change on account of a 
    //      clock_set as "do not alter" 
    curipm.datasource    = "";
    curipm.j             = -1;
    curipm.tvg           = -1;
    curipm.selpps        = -1;
    curipm.bitstreammask =  0;
    curipm.clockfreq     =  0;  // this one _may_ be set later on

    // If there is a frequency given, inspect it and transform it
    // to a 'k' value [and see if that _can_ be done!]
    int             k;
    string          warning;
    samplerate_type f_req;
    istringstream   iss( args[1] + (args[1].find('/')==string::npos ? "/1" : "") );

    iss >> f_req;

    // can only do 2,4,8,16,32,64 MHz
    EZASSERT2(f_req>=2 && f_req<=64, Error_Code_8_Exception,
              EZINFO(" Requested frequency " << f_req << " <2 or >64 is not allowed") );

    // Assert if power of 2
    EZASSERT2(f_req.denominator()==1 && (f_req.numerator() & (f_req.numerator()-1))==0, Error_Code_8_Exception,
              EZINFO(" Requested frequency " << f_req << " is not a power of 2") );

    k         = (int)::round( ::log( boost::rational_cast<double>(f_req) )/M_LN2 ) - 1;

    // The "k" value is one of the required parameters
    // and go to MHz
    curipm.k          = k;
    curipm.clockfreq  = f_req * 1000000;

    // We already verified that the clocksource is 'int' or 'ext'
    // 64MHz *implies* using the external VSI clock; the on-board
    // clockgenerator can only do 40MHz
    // If the user says '64MHz' with 'internal' clock we just warn
    // him/her ...
    curipm.selcgclk = (args[2]=="int");

    // We do not alter the programmed clockfrequency, unless the
    // usr requests we do (if there is a 3rd argument,
    // it's the clock-generator's clockfrequency)
    // HV: 24-sep-2013  Add the condition that the clock source be "int"
    if( args.size()>=4 && !args[3].empty() ) {
        // if clock source is NOT int, warn that setting this frequency is
        // a no-op.
        // TODO FIXME Realistically this could be considered an error!
        if( !curipm.selcgclk )
            warning = string(" : WARN - ignoring internal clock freq because of clock source 'ext'")+warning;
        // for backwards compatibility we allow programming the internal
        // clock generator frequency under all circumstances
        istringstream iss2( args[3] + (args[3].find('/')==string::npos ? "/1" : "") );

        iss2 >> curipm.clockfreq;
        curipm.clockfreq *= 1000000;
    }

    // HV: 18-Nov-2013 Decided to *force* the use of only DOT1PPS in stead
    //                 of alternating DIM1PPS if external clock.
    //                 The reason is that we now have only ONE 1PPS interrupt
    //                 source and the time keeping will always be
    //                 consistent, no matter wether the clock is taken from
    //                 the clock generator chip or from the external VSI
    //                 source
    curipm.seldim = false;
    curipm.seldot = true;
    // Send to hardware
    rte.set_input( curipm );

    reply << " 0" << warning << " ;";
    return reply.str();
}
