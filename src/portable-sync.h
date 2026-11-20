#pragma once
#ifdef __cplusplus
extern "C" {
#endif
struct framerot_mutex;
struct framerot_mutex *framerot_mutex_create(void);
void framerot_mutex_destroy(struct framerot_mutex *);
void framerot_mutex_lock(struct framerot_mutex *);
void framerot_mutex_unlock(struct framerot_mutex *);
/* Single immutable MPEG-4 VLC lookup-table initialization per module. */
void framerot_init_vlc_once(void (*init)(void));
#ifdef __cplusplus
}
#endif
