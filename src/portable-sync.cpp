// GPL-2.0; see COPYING.
#include "portable-sync.h"
#include <mutex>
struct framerot_mutex { std::mutex value; };
framerot_mutex *framerot_mutex_create(void)
{
    try {return new framerot_mutex;} catch(...) {return nullptr;}
}
void framerot_mutex_destroy(framerot_mutex *m) {delete m;}
void framerot_mutex_lock(framerot_mutex *m) {m->value.lock();}
void framerot_mutex_unlock(framerot_mutex *m) {m->value.unlock();}
void framerot_init_vlc_once(void (*init)(void))
{
    static std::once_flag once;
    std::call_once(once,init);
}
