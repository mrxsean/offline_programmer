#pragma once

/* Called only by the HTTP server task. Caller frees the returned JSON string. */
char *io_debug_execute(const char *request);
