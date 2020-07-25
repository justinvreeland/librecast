#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include "log.h"
#include "misc.h"

unsigned int LOG_LEVEL = 127;

void logmsg(int level, char *msg, ...)
{
	va_list argp;
	char *b;

	if ((LOG_LEVEL & level) != level) return;
	va_start(argp, msg);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	b = malloc(_vscprintf(msg, argp) + 1);
	assert(b != NULL);
	vsprintf(b, msg, argp);
	va_end(argp);
	fprintf(stderr, "%s\n", b);
	free(b);
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
}
