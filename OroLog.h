// ============================================================================
// OroLog.h - ORO writes to Orbiter.log through HERE, and nowhere else.
// ----------------------------------------------------------------------------
// WHY THIS EXISTS. The 260906 release shipped a stale SetFloat: the client logged
// one D3D9ERROR per frame for it, and a twenty-minute flight produced a 17,134-line
// Orbiter.log of which 16,252 lines were that one error. His ruling:
//
//     "No per frame writing on the log, especially on the release. At most, we
//      write a line per real second (not simulation seconds), if we want to hunt
//      for something."
//
// THE LEVELS, his design. `Debug` in Config\ORO.cfg:
//
//   0  only what is absolutely necessary - failures, and the handful of lines that
//      say whether the addon is alive and what the client can do. A user running
//      quiet still gets enough to diagnose a broken install.
//   1  CONCISE, and the DEFAULT: the above plus one line per subsystem at startup.
//      This is what a tester's log should look like, and what ships.
//   2  VERBOSE - for actually hunting something. Anything that samples state over
//      time lives here and NOWHERE ELSE.
//
// A level is a CEILING, not a category: OroLog(0, ...) is always written, and
// OroLog(2, ...) only when the user has asked for it.
//
// ⚠️ THE RATE RULE IS SEPARATE FROM THE LEVEL, and it is absolute: nothing may be
// written more than ONCE PER REAL SECOND, at any level. Use ORO_LOG_EVERY for
// anything inside a function that can run per frame. It keeps its own timer per
// call site, off a real-time tick that keeps running while the sim is PAUSED and
// does not care about time acceleration - sim time would fire every frame at 100x.
//
// ⚠️ AND NEITHER OF THEM MAKES THIS SAFE IN THE RENDER CALLBACK. oapiWriteLog is an
// oapi call, and invariant 1 says the render path makes none. Record what you want
// to say and let the next main-thread pass write it - the lightning's first-flash
// breadcrumb and both pool-full warnings all work that way.
//
// tools/logaudit.py enforces all of this: a raw oapiWriteLog, or an unguarded call
// in a per-frame function, fails it.
// ============================================================================
#ifndef __ORO_LOG_H
#define __ORO_LOG_H

// Write to Orbiter.log if 'level' is within the user's Debug setting.
// Printf-style. Main thread only (see the note above about the render path).
void OroLog(int level, const char* fmt, ...);

// A real-time millisecond tick that is safe anywhere - no oapi call, unaffected by
// pause or time acceleration. Declared here so this header needs no <windows.h>.
unsigned long OroLogTick();

// One line per real second, per call site. The static lives in the macro so two
// call sites can never share a timer; unsigned arithmetic makes the 49-day tick
// wrap a non-event, and a zero-initialised timer logs the first call immediately.
#define ORO_LOG_EVERY(level, ...)                                      \
    do {                                                               \
        static unsigned long s_oroLogT_ = 0;                           \
        const unsigned long t_ = OroLogTick();                         \
        if ((unsigned long)(t_ - s_oroLogT_) >= 1000UL) {              \
            s_oroLogT_ = t_;                                           \
            OroLog(level, __VA_ARGS__);                                \
        }                                                              \
    } while (0)

#endif // __ORO_LOG_H
