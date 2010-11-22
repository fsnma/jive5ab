// Multithread processing chain building blocks
// Copyright (C) 2007-2008 Harro Verkouter
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
#include <chain.h>
#include <dosyscall.h>

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>

using namespace std;

// Implement the exception class
DEFINE_EZEXCEPT(chainexcept);

// Reserve space for the static datamember
thunk_type chain::nop = thunk_type();

//  The chain interface
chain::chain() :
    _chain( new chainimpl() )
{}

// Run without any userarguments
void chain::run() {
    _chain->run();
}

void chain::wait() {
    return _chain->join_and_cleanup();
}

void chain::stop() {
    return _chain->stop();
}

void chain::gentle_stop() {
    return _chain->gentle_stop();
}

bool chain::empty(void) const {
    return _chain->empty();
}

chain::~chain() {}




//
//
//    IMPLEMENTATION CLASSES
//
//
chain::internalstep::internalstep(const string& udtp, thunk_type* oqdisabler,
                                  thunk_type* iqdisabler, unsigned int sid, unsigned int n):
    qdepth(0), stepid(sid), nthread(n), actualudptr(0), udtype(udtp),
    rsa(&threadfn, oqdisabler,iqdisabler)
{

    PTHREAD_CALL( ::pthread_mutex_init(&mutex, 0) );
    PTHREAD_CALL( ::pthread_cond_init(&condition, 0) );
}

// only called when step is dead.
// destroy all resourcen
chain::internalstep::~internalstep() {
    // delete dynamically allocated thingies
    uddeleter(actualudptr);
    iqdeleter();
    oqdeleter();
    sdeleter();
    // erase all thunks/curries
    setud.erase();
    setqd.erase();
    setstepid.erase();
    sdeleter.erase();
    iqdeleter.erase();
    oqdeleter.erase();
    udmaker.erase();
    uddeleter.erase();
    threadfn.erase();
    // destroy the mutex & conditionvar
    ::pthread_mutex_destroy(&mutex);
    ::pthread_cond_destroy(&condition);
}


chain::internalq::internalq(const string& tp):
    actualqptr(0), elementtype(tp)
{}

chain::internalq::~internalq() {
    qdeleter();
    // erase all thunks/curries
    enable.erase();
    disable.erase();
    delayed_disable.erase();
    qdeleter.erase();
}

// The stepfn_type: combines a pointer to an actual step
// and a set of functionpointers. When called, the 
// steps' mutex will be held, the function executed,
// the condition variable will be broadcast and finally,
// the mutex will be released again
chain::stepfn_type::stepfn_type(stepid s, curry_type ct):
    step(s), calldef(ct)
{}



//
// the actual chainimpl
//


chain::chainimpl::chainimpl() :
    closed(false), running(false)
{}

// Only allow a chain to run if it's closed and not running yet.
void chain::chainimpl::run() {
    EZASSERT(closed==true && running==false, chainexcept);

    // Great. Now we begin running the chain,
    // setting it up from the end. We can nicely
    // Use the std::vector reverse iterators here.
    sigset_t                       oldset, newset;
    unsigned int                   n;
    ostringstream                  err;
    pthread_attr_t                 attribs;
    struct sched_param             parms;
    steps_type::reverse_iterator   sptrptr;
    queues_type::reverse_iterator  qptrptr;

    // Set the thread attributes for our kinda threads
    PTHREAD_CALL( ::pthread_attr_init(&attribs) );
    PTHREAD_CALL( ::pthread_attr_setdetachstate(&attribs, PTHREAD_CREATE_JOINABLE) );
    // Only set scheduling policy if we have root?
    if( ::geteuid()==0 ) {
        PTHREAD_CALL( ::pthread_attr_setschedpolicy(&attribs, SCHED_RR) );
        parms.sched_priority = sched_get_priority_max(SCHED_RR);
        PTHREAD_CALL( ::pthread_attr_setschedparam(&attribs, &parms) );
    }

    // Make the threads start with all signals blocked. Then,
    // each thread can decide if and if so which signals are 
    // to be unblocked.
    EZASSERT(sigfillset(&newset)==0, chainexcept);
    PTHREAD_CALL( ::pthread_sigmask(SIG_SETMASK, &newset, &oldset) );

    // Since the chain is closed, we are sure that there ARE
    // elements in the steps- and queues vectors, hence can
    // do the following unconditionally.
    
    // Enable all queues
    for(qptrptr=queues.rbegin(); qptrptr!=queues.rend(); qptrptr++)
        (*qptrptr)->enable();

    // Note: the variable "n" is initialized inside the loop
    // to the number of threads that need to be created.
    // At the end of the loop the value is tested againt 0
    // since if it is NOT 0, this means at least one of the
    // threads couldn't be created.
    try {
        for(sptrptr=steps.rbegin(), n=0; n==0 && sptrptr!=steps.rend(); sptrptr++) {
            internalstep* is = (*sptrptr);

            // Create a new UserData thingy.
            // First call the UserData-maker thunk
            is->udmaker();
            // Call upon another curried thing to transform it to void*
            // (it typesafely extracts "UserData*" and casts to void*
            is->udtovoid(&is->udmaker);
            // now we can extract the void* from that one.
            // Nice thing is that at this point we have NO clue about
            // what the actual type of UserData IS (nor do we NEED to know)
            // We DO need to store the actual pointer.
            is->udtovoid.returnval(is->actualudptr);

            // Update the steps' sync_data<UserData> with the new
            // UserDataptr. Also make sure the cumulative queuedepth
            // gets set. This never changes once the chain is closed, however, 
            // we do not know this value UNTIL the chain is closed, and not
            // at the time of adding the step. By always setting
            // it, we don't care when we finally knew what value it was -
            // it will always be set BEFORE the thread will actually
            // be able to access the value.
            is->setud(is->actualudptr);
            is->setqd(is->qdepth);
            is->setstepid(is->stepid);
            is->setcancel(false);

            // Create the threads!
            // Also store the number of threads in the runstepargs 
            // structure so terminating threads know when they are
            // last one to leave
            n               = is->nthread;
            is->rsa.nthread = is->nthread;
            while( n ) {
                int         rv;
                pthread_t*  tidptr = new pthread_t;

                // Only add the threadid if succesfully created!
                rv = ::pthread_create(tidptr, &attribs, &chain::run_step, &is->rsa); 
                if( rv!=0 ) {
                    err << "chain/run: failed to create thread: " << ::strerror(rv);
                    break;
                }
                is->threads.push_back(tidptr);
                n--;
            }
        }
    }
    catch( const std::exception& e ) {
        err << "chain/run: " << e.what();
    }
    catch( ... ) {
        err << "chain/run: caught deadly unknown exception" << endl;
    }
    // Restore the old signalmask
    PTHREAD_CALL( ::pthread_sigmask(SIG_SETMASK, &oldset, 0) );
    // Do some cleanup
    PTHREAD_CALL( ::pthread_attr_destroy(&attribs) );

    // If we didn't reach the end of the chain something went wrong
    // and we stop the chain (all the ones we've started so far).
    // Preset running to true - in case of error "->stop()" will reset it
    // back to false after finishing.
    running = true;
    if( sptrptr!=steps.rend() ) {
        cerr << "chain/run: failed to start the chain. Stopping." << endl
             << err.str() << endl;
        this->stop();
    }
    EZASSERT2(this->running==true, chainexcept, EZINFO(err.str()));
}

// delayed-disable the first queue and all other steps
// should finish cleanly. Much less brutal than stop().
// You must be reasonably sure that your chain adheres
// to the bqueue-disabled semantics for detecting
// a stop ....
// Registered cancellationroutines are first called
void chain::chainimpl::gentle_stop() {
    if( !running )
        return;
    if( !closed )
        return;

    // first do the user-registered cancellations
    this->do_cancellations();
    // now those of the sync_type<>'s
    this->cancel_synctype();

    (*queues.begin())->delayed_disable();

    this->join_and_cleanup();
    return;
}

// Unconditionally returns the chain to clean state.
// The steps are kept but all threads are stopped
// and the runtime-info is cleared such it may be run again
// at a later stage.
void chain::chainimpl::stop() {
    if( !this->running )
        return;
    if( !this->closed )
        return;

    // Ok, there is something to stop
    queues_type::iterator qptrptr;

    // first do the user-registered cancellations
    this->do_cancellations();

    // Inform the sync_type<>'s of the cancellation
    this->cancel_synctype();

    // now bluntly disable ALL queues
    for(qptrptr=queues.begin(); qptrptr!=queues.end(); qptrptr++) 
        (*qptrptr)->disable();

    // sais it all ...
    this->join_and_cleanup();
}

bool chain::chainimpl::empty(void) const {
    return this->steps.empty();
}

// Loop over all steps and set the
// "cancel" flag for them to true
void chain::chainimpl::cancel_synctype() {
    stepid  s;

    for(s=0; s<steps.size(); s++) 
        this->communicate(s, makethunk(steps[s]->setcancel, true));
}

void chain::chainimpl::join_and_cleanup() {
    void*                   voidptr;
    steps_type::iterator    sptrptr;

    // Join everyone - all cancellations should have been processed
    // and at least of the queues was (delayed)disabled, triggering
    // a chain of delayed disables.
    //
    // Loop over all steps and for each step, join all
    // threads that were executing that step
    for(sptrptr=steps.begin(); sptrptr!=steps.end(); sptrptr++) {
        tid_type::iterator  thrdidptrptr;

        for(thrdidptrptr = (*sptrptr)->threads.begin();
            thrdidptrptr != (*sptrptr)->threads.end();
            thrdidptrptr++)
                ::pthread_join(**thrdidptrptr, &voidptr);
    }

    // Great. All threads have been joined. Time to clean up.
    // All the userdata must go.
    for(sptrptr=steps.begin(); sptrptr!=steps.end(); sptrptr++) {
        internalstep*   sptr = (*sptrptr);

        // call the registered delete function and indicate
        // deletion by setting the actualudptr to 0
        sptr->uddeleter(sptr->actualudptr);
        sptr->actualudptr = 0;
    }

    // And the queue is not running anymore
    running = false;
    return;
}

// Process all cancellations - that is, if we're running
void chain::chainimpl::do_cancellations() {
    if( !running )
        return;

    // Loop over all the cancellations
    cancellations_type::iterator  cancel;
    for( cancel=cancellations.begin();
         cancel!=cancellations.end();
         cancel++ )
            this->communicate(cancel->step, cancel->calldef);
}

// Execute some function on the userdata for step 's'.
// It does it with the mutex held and the condition
// will be automatically broadcasted.
void chain::chainimpl::communicate(stepid s, curry_type ct) {
    // Make sure we can sensibly execute the code
    // Separate the clauses such that in case of error, the user
    // actually knows which one was the culprit
    EZASSERT(running==true, chainexcept);
    EZASSERT(s<steps.size(), chainexcept);
    // Assert that the argument of the curried thing
    // equals the argument of the step it wants to
    // execute on/with. Typically it is of type "pointer-to-userdata",
    // ie: the curry_type must take a pointer-to-userdata as
    // single argument.
    thunk_type     thunk;
    internalstep*  isptr = steps[s];

    if(isptr->actualudptr==0) {
        cerr << "chain/communicate: step[" << s << "] has no userdata. No communication." << endl;
        return;
    }
    EZASSERT(ct.argumenttype()==isptr->udtype, chainexcept);
    thunk = makethunk(ct, isptr->actualudptr);
    this->communicate(s, thunk);
    thunk.erase();
}

void chain::chainimpl::communicate(stepid s, thunk_type tt) {
    // Make sure we can sensibly execute the code
    // Separate the clauses such that in case of error, the user
    // actually knows which one was the culprit
    EZASSERT(running==true, chainexcept);
    EZASSERT(s<steps.size(), chainexcept);
    // Grab hold of the correct step, so's we have access
    // to the mutex & condition variable
    internalstep*  isptr = steps[s];

    ::pthread_mutex_lock(&isptr->mutex);
    // Make sure that even if the functioncall throws
    // we do not end up with a locked mutex
    try {
        // call the thunk'ed function
        tt();
    }
    catch( std::exception& e ) {
        cerr << "chain/communicate: **ouch**! " << endl
             << "   " << e.what() << endl;
    }
    catch( ... ) {
        cerr << "chain/communicate: unknown exception." << endl;
    }
    // Ok, now we can broadcasts (if no-one was waiting for
    // the condition variable it is a no-op, otherwise those
    // waiting for it may need to re-evaluate their condition)
    ::pthread_cond_broadcast(&isptr->condition);
    ::pthread_mutex_unlock(&isptr->mutex);
}

void chain::chainimpl::register_cancel(stepid stepnum, curry_type ct) {
    // Assert it's safe to register this'un.
    // Separate the clauses such that in case of error, the user
    // actually knows which one was the culprit
#if 0
    // Only allow registering
    // cancel methods after the chain is closed.
    EZASSERT(closed==true, chainexcept);
#endif
    EZASSERT(stepnum<steps.size(), chainexcept);
    // Verify the functioncall might succeed ...
    EZASSERT(ct.argumenttype()==steps[stepnum]->udtype, chainexcept);

    cancellations.push_back( stepfn_type(stepnum, ct) );
}

// If the chainimpl is to be deleted, this means no-one's referencing
// us anymore. This in turn means that the steps and the queues may
// go; we cannot be restarted ever again.
chain::chainimpl::~chainimpl() {
    // Make sure we're stopped ...
    this->stop();

    // Now delete all steps and queues
    steps_type::iterator   sptrptr;
    for(sptrptr=steps.begin(); sptrptr!=steps.end(); sptrptr++)
        delete (*sptrptr);

    queues_type::iterator  qptrptr;
    for(qptrptr=queues.begin(); qptrptr!=queues.end(); qptrptr++) 
        delete (*qptrptr);

    // And erase all registered cancellations
    cancellations_type::iterator  cancel;
    for( cancel=cancellations.begin();
         cancel!=cancellations.end();
         cancel++ )
        cancel->calldef.erase();
}

chain::runstepargs::runstepargs(thunk_type* tttptr,
                                thunk_type* ddoptr,
                                thunk_type* diptr):
    threadthunkptr(tttptr), delayeddisableoutqptr(ddoptr),
    disableinqptr(diptr), nthread(0)
{
    PTHREAD_CALL( ::pthread_mutex_init(&mutex, 0) );
}

// Run a thunk in a thread. Hoopla.
void* chain::run_step(void* runstepargsptr) {
    runstepargs*  rsaptr = (runstepargs*)runstepargsptr;
    
    try {
        (*rsaptr->threadthunkptr)();
    }
    catch( const std::exception& e ) {
        cerr << "OH NOES! A step threw an exception:" << endl
             << "**** " << e.what() << endl;
    }
    catch( ... ) {
        cerr << "OH NOES! A step threw an unknown exception!" << endl;
    }
    // And now delayed-disable this ones' output queue.
    // And bluntly disable the input q.
    // The fact that WE are executing this code means that WE
    // have stopped popping stuff from an inputqueue.
    // If we are the last thread that stopped popping, better inform
    // the step upstream that no-one is popping anymore.
    // IF our inputQ WAS already (delayed)disabled then this adds nothing,
    // however, if it was NOT it allows the "stopsignal" also to
    // go upstream rather than just downstream.
    unsigned int  n;

    // Hold the mutex for as short as possible.
    // Copy out the updated "rsaptr->nthread" value.
    PTHREAD_CALL( ::pthread_mutex_lock(&rsaptr->mutex) );
    if( rsaptr->nthread )
        rsaptr->nthread--;
    n = rsaptr->nthread;
    PTHREAD_CALL( ::pthread_mutex_unlock(&rsaptr->mutex) );

    // If we were last ... do the signalling!
    if( n==0 ) {
        (*rsaptr->delayeddisableoutqptr)();
        (*rsaptr->disableinqptr)();
    }
    return (void*)0;
}