#ifndef UPDATER_H
#define UPDATER_H

/* App Museum II update check (appcatalog.webosarchive.org). A background
 * thread fetches the latest version at startup; the main loop polls
 * updater_has_update() and shows the prompt screen when one is found. */

void updater_check_start(void);   /* spawn the check thread (idempotent) */
int  updater_has_update(void);    /* newer version found and not dismissed */
const char *updater_get_version(void);
int  updater_install(void);       /* hand off to Preware; 1 = launched, caller should exit */
void updater_dismiss(void);

#endif
