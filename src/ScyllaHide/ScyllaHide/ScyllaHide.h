#ifndef _SCYLLAHIDE_H_
#define _SCYLLAHIDE_H_

#include <Windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void scylla_hide_setup();
void scylla_hide_on_stop();
void scylla_hide_on_debug();
void scylla_hide_on_create_process(DWORD pid, void*);
void scylla_hide_on_create_dll(void*);
void scylla_hide_on_breakpoint();

struct scl_settings *scylla_hide_get_settings(void);

#ifdef __cplusplus
}
#endif

#endif // _SCYLLAHIDE_H_
