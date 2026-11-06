/*
 * SAB - native script extensions of the 8025_webm engine.
 * Script profiler hooks, called by the interpreter around every CALL.
 *
 * Compiled only with -DSAB (engine 8025_webm); without it the hooks vanish.
 */
#ifndef SAB_PROF_H
#define SAB_PROF_H

#ifdef SAB
extern int sab_prof_on;
struct Interpreter;
struct Instruction;
void sab_prof_enter_call(void *interpreter, void *call);
void sab_prof_leave(void);
/* call: the CALL instruction (script target or native function) */
#define SAB_PROF_ENTER(interp, call) do { if(sab_prof_on) sab_prof_enter_call(interp, call); } while(0)
#define SAB_PROF_LEAVE()     do { if(sab_prof_on) sab_prof_leave(); } while(0)
#else
#define SAB_PROF_ENTER(interp, call) do { } while(0)
#define SAB_PROF_LEAVE()     do { } while(0)
#endif

#endif
