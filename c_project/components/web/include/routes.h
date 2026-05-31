#ifndef WEB_ROUTES_H
#define WEB_ROUTES_H

/**
 * Register all application routes with the web server.
 *
 * Must be called after webserver module is initialized but before
 * webserver_start().
 */
void routes_register_all(void);

#endif /* WEB_ROUTES_H */
